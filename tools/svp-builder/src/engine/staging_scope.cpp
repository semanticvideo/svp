#include "engine/staging_scope.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace svp::builder::engine {
namespace fs = std::filesystem;

namespace {

std::string relative_generic(const fs::path& staging_dir, const fs::path& path) {
  return fs::relative(path, staging_dir).generic_string();
}

std::vector<std::byte> read_file_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read staging file: " + path.string());
  }
  const std::string content{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(content.size());
  std::transform(content.begin(), content.end(), bytes.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  return bytes;
}

void write_file_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write staging file: " + path.string());
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write staging file: " + path.string());
  }
}

}  // namespace

bool scope_covers(const StagingScope& scope, std::string_view relative_path) {
  return std::any_of(scope.prefixes.begin(), scope.prefixes.end(),
                     [&](const std::string& prefix) {
                       return relative_path.starts_with(prefix);
                     });
}

bool scopes_overlap(const StagingScope& left, const StagingScope& right) {
  for (const std::string& a : left.prefixes) {
    for (const std::string& b : right.prefixes) {
      if (a.starts_with(b) || b.starts_with(a)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> list_staging_entries(const fs::path& staging_dir) {
  std::vector<std::string> entries;
  std::error_code error;
  if (!fs::is_directory(staging_dir, error)) {
    return entries;
  }
  for (const auto& entry : fs::recursive_directory_iterator(staging_dir)) {
    if (entry.is_directory()) {
      entries.push_back(relative_generic(staging_dir, entry.path()) + "/");
    } else if (entry.is_regular_file()) {
      entries.push_back(relative_generic(staging_dir, entry.path()));
    }
  }
  std::sort(entries.begin(), entries.end());
  return entries;
}

std::vector<StagedEntry> capture_staging_scope(const fs::path& staging_dir,
                                               const StagingScope& scope) {
  std::vector<StagedEntry> captured;
  for (const std::string& relative : list_staging_entries(staging_dir)) {
    if (!scope_covers(scope, relative)) {
      continue;
    }
    StagedEntry entry;
    entry.relative_path = relative;
    if (relative.ends_with('/')) {
      entry.kind = StagedEntryKind::directory;
    } else {
      entry.bytes = read_file_bytes(staging_dir / relative);
    }
    captured.push_back(std::move(entry));
  }
  return captured;
}

void restore_staging_scope(const fs::path& staging_dir, const StagingScope& scope,
                           const std::vector<StagedEntry>& entries) {
  std::vector<std::string> wanted;
  wanted.reserve(entries.size());
  for (const StagedEntry& entry : entries) {
    wanted.push_back(entry.relative_path);
  }
  std::sort(wanted.begin(), wanted.end());

  // Remove what the scope holds but the capture does not: files first, then
  // directories deepest first, and only once empty.
  std::vector<std::string> existing = list_staging_entries(staging_dir);
  for (const std::string& relative : existing) {
    if (!relative.ends_with('/') && scope_covers(scope, relative) &&
        !std::binary_search(wanted.begin(), wanted.end(), relative)) {
      fs::remove(staging_dir / relative);
    }
  }
  for (auto it = existing.rbegin(); it != existing.rend(); ++it) {
    if (it->ends_with('/') && scope_covers(scope, *it) &&
        !std::binary_search(wanted.begin(), wanted.end(), *it)) {
      std::error_code error;
      if (fs::is_empty(staging_dir / *it, error)) {
        fs::remove(staging_dir / *it, error);
      }
    }
  }

  for (const StagedEntry& entry : entries) {
    if (!scope_covers(scope, entry.relative_path)) {
      throw std::runtime_error("captured staging entry outside its task's scope: " +
                               entry.relative_path);
    }
    if (entry.kind == StagedEntryKind::directory) {
      fs::create_directories(staging_dir / entry.relative_path);
    } else {
      write_file_bytes(staging_dir / entry.relative_path, entry.bytes);
    }
  }
}

}  // namespace svp::builder::engine
