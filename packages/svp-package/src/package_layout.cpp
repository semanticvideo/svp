#include "svp/package/package_layout.hpp"

#include <zip.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace svp::package {
namespace {

struct ZipDeleter {
  void operator()(zip_t* archive) const noexcept {
    if (archive != nullptr) {
      zip_discard(archive);
    }
  }
};

using ZipArchive = std::unique_ptr<zip_t, ZipDeleter>;

std::string zip_error_message(int error_code) {
  zip_error_t error;
  zip_error_init_with_code(&error, error_code);
  std::string message = zip_error_strerror(&error);
  zip_error_fini(&error);
  return message;
}

bool is_normalized_package_path(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) {
    return false;
  }

  std::size_t start = 0;
  while (start < path.size()) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string_view::npos
                                             ? std::string_view::npos
                                             : slash - start);

    if (part.empty()) {
      return slash == path.size() - 1;
    }

    if (part == "." || part == "..") {
      return false;
    }

    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }

  return true;
}

std::string root_entry_for(std::string_view entry) {
  const auto slash = entry.find('/');
  if (slash == std::string_view::npos) {
    return std::string{entry};
  }
  return std::string{entry.substr(0, slash)};
}

}  // namespace

bool PackageLayout::has_entry(const std::string& entry) const {
  return entries.contains(entry);
}

bool PackageLayout::has_top_level_section(const std::string& section) const {
  return root_entries.contains(section);
}

PackageLayoutResult PackageLayoutResult::success(PackageLayout layout) {
  PackageLayoutResult result;
  result.layout_ = std::move(layout);
  result.has_value_ = true;
  return result;
}

PackageLayoutResult PackageLayoutResult::failure(std::string message) {
  PackageLayoutResult result;
  result.error_message_ = std::move(message);
  return result;
}

PackageEntryReadResult PackageEntryReadResult::success(std::string content) {
  PackageEntryReadResult result;
  result.content_ = std::move(content);
  result.has_value_ = true;
  return result;
}

PackageEntryReadResult PackageEntryReadResult::failure(std::string message) {
  PackageEntryReadResult result;
  result.error_message_ = std::move(message);
  return result;
}

bool PackageEntryReadResult::has_value() const noexcept {
  return has_value_;
}

const std::string& PackageEntryReadResult::value() const {
  if (!has_value_) {
    throw std::logic_error("package entry read result has no value");
  }
  return content_;
}

const std::string& PackageEntryReadResult::error_message() const noexcept {
  return error_message_;
}

bool PackageLayoutResult::has_value() const noexcept {
  return has_value_;
}

const PackageLayout& PackageLayoutResult::value() const {
  if (!has_value_) {
    throw std::logic_error("package layout result has no value");
  }
  return layout_;
}

const std::string& PackageLayoutResult::error_message() const noexcept {
  return error_message_;
}

PackageLayoutResult read_package_layout(const std::filesystem::path& path) {
  int error_code = ZIP_ER_OK;
  ZipArchive archive{zip_open(path.string().c_str(), ZIP_RDONLY, &error_code)};
  if (!archive) {
    return PackageLayoutResult::failure(zip_error_message(error_code));
  }

  const auto entry_count = zip_get_num_entries(archive.get(), 0);
  if (entry_count < 0) {
    return PackageLayoutResult::failure("could not read ZIP entry count");
  }

  PackageLayout layout;
  for (zip_int64_t index = 0; index < entry_count; ++index) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(index), 0, &stat) != 0) {
      return PackageLayoutResult::failure(zip_strerror(archive.get()));
    }

    if (stat.name == nullptr) {
      return PackageLayoutResult::failure("ZIP entry has no name");
    }

    const std::string entry_name{stat.name};
    if (!is_normalized_package_path(entry_name)) {
      layout.invalid_entry_paths.push_back(entry_name);
      continue;
    }

    layout.entries.insert(entry_name);
    layout.root_entries.insert(root_entry_for(entry_name));
  }

  return PackageLayoutResult::success(std::move(layout));
}

PackageEntryReadResult read_package_entry(const std::filesystem::path& path,
                                          const std::string& entry) {
  int error_code = ZIP_ER_OK;
  ZipArchive archive{zip_open(path.string().c_str(), ZIP_RDONLY, &error_code)};
  if (!archive) {
    return PackageEntryReadResult::failure(zip_error_message(error_code));
  }

  zip_stat_t stat;
  zip_stat_init(&stat);
  if (zip_stat(archive.get(), entry.c_str(), 0, &stat) != 0) {
    return PackageEntryReadResult::failure(zip_strerror(archive.get()));
  }

  if ((stat.valid & ZIP_STAT_SIZE) == 0) {
    return PackageEntryReadResult::failure("ZIP entry size is unavailable");
  }

  std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file{
      zip_fopen(archive.get(), entry.c_str(), 0), zip_fclose};
  if (!file) {
    return PackageEntryReadResult::failure(zip_strerror(archive.get()));
  }

  std::string content;
  content.resize(static_cast<std::size_t>(stat.size));

  std::size_t offset = 0;
  while (offset < content.size()) {
    const auto bytes_read =
        zip_fread(file.get(), content.data() + offset, content.size() - offset);
    if (bytes_read < 0) {
      return PackageEntryReadResult::failure(zip_file_strerror(file.get()));
    }
    if (bytes_read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(bytes_read);
  }

  if (offset != content.size()) {
    return PackageEntryReadResult::failure("ZIP entry ended before declared size");
  }

  return PackageEntryReadResult::success(std::move(content));
}

}  // namespace svp::package
