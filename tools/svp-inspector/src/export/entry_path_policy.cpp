#include "entry_path_policy.hpp"

#include "export_error.hpp"

#include <climits>
#include <map>
#include <string>

namespace package_export {
namespace {

// ASCII control characters (C0 and DEL) are never valid in a mirrored path.
constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr unsigned char kAsciiDelete = 0x7f;

[[noreturn]] void throw_unsafe(std::string_view name, std::string reason) {
  throw ExportError(ExportErrorCode::unsafe_entry_name,
                    "Package entry name cannot be exported safely: " + reason,
                    entry_details(name));
}

std::string ascii_lowercase(std::string_view text) {
  std::string folded{text};
  for (auto& character : folded) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return folded;
}

}  // namespace

void require_safe_entry_name(std::string_view name, bool is_directory) {
  if (name.empty()) {
    throw_unsafe(name, "the name is empty");
  }
  if (name.front() == '/') {
    throw_unsafe(name, "the name is absolute");
  }
  for (const char character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < kFirstPrintableAscii || byte == kAsciiDelete) {
      throw_unsafe(name, "the name contains a control character");
    }
    if (character == '\\') {
      throw_unsafe(name, "the name contains a backslash");
    }
  }

  std::string_view remaining = name;
  if (is_directory && remaining.back() == '/') {
    remaining.remove_suffix(1);
  }
  std::size_t start = 0;
  while (start <= remaining.size()) {
    const auto slash = remaining.find('/', start);
    const auto end = slash == std::string_view::npos ? remaining.size() : slash;
    const auto segment = remaining.substr(start, end - start);
    if (segment.empty()) {
      throw_unsafe(name, "the name has an empty path segment");
    }
    if (segment == "." || segment == "..") {
      throw_unsafe(name, "the name has a '.' or '..' path segment");
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
}

void require_output_segments_fit(std::string_view output_path,
                                 std::string_view entry) {
  std::size_t start = 0;
  while (start <= output_path.size()) {
    const auto slash = output_path.find('/', start);
    const auto end = slash == std::string_view::npos ? output_path.size() : slash;
    if (end - start > NAME_MAX) {
      throw_unsafe(entry, "an exported path segment is longer than NAME_MAX");
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
}

void require_no_output_collisions(const std::vector<PlannedOutputPath>& files) {
  std::map<std::string, const PlannedOutputPath*> by_folded_path;
  for (const auto& file : files) {
    const auto [existing, inserted] =
        by_folded_path.emplace(ascii_lowercase(file.path), &file);
    if (!inserted) {
      throw ExportError(
          ExportErrorCode::output_path_collision,
          "Two package entries would be exported to the same file.",
          nlohmann::json{{"entry", file.entry},
                         {"other_entry", existing->second->entry},
                         {"path", file.path}});
    }
  }
  for (const auto& file : files) {
    const auto folded = ascii_lowercase(file.path);
    for (auto slash = folded.find('/'); slash != std::string::npos;
         slash = folded.find('/', slash + 1)) {
      const auto parent = by_folded_path.find(folded.substr(0, slash));
      if (parent != by_folded_path.end()) {
        throw ExportError(
            ExportErrorCode::output_path_collision,
            "A package entry would be exported to a path another entry "
            "needs as a directory.",
            nlohmann::json{{"entry", file.entry},
                           {"other_entry", parent->second->entry},
                           {"path", parent->second->path}});
      }
    }
  }
}

}  // namespace package_export
