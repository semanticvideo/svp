#include "package_reader.hpp"

#include "export_error.hpp"

#include <zip.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace package_export {
namespace {

std::string zip_error_text(int error_code) {
  zip_error_t error;
  zip_error_init_with_code(&error, error_code);
  std::string message = zip_error_strerror(&error);
  zip_error_fini(&error);
  return message;
}

[[noreturn]] void throw_unreadable(std::string_view entry,
                                   std::string message) {
  throw ExportError(ExportErrorCode::entry_unreadable,
                    "Package entry could not be read: " + message,
                    entry_details(entry));
}

[[noreturn]] void throw_size_mismatch(std::string_view entry,
                                      std::string message) {
  throw ExportError(ExportErrorCode::entry_size_mismatch, std::move(message),
                    entry_details(entry));
}

zip_t* open_archive(const std::filesystem::path& path,
                    const std::optional<PackageByteRange>& range) {
  if (!range.has_value()) {
    int error_code = ZIP_ER_OK;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error_code);
    if (archive == nullptr) {
      throw_unreadable("", zip_error_text(error_code));
    }
    return archive;
  }

  if (range->size >
      static_cast<std::uint64_t>(std::numeric_limits<zip_int64_t>::max())) {
    throw_unreadable("", "embedded package byte range exceeds ZIP limits");
  }
  zip_error_t error;
  zip_error_init(&error);
  zip_source_t* source = zip_source_file_create(
      path.c_str(), static_cast<zip_uint64_t>(range->offset),
      static_cast<zip_int64_t>(range->size), &error);
  if (source == nullptr) {
    std::string message = zip_error_strerror(&error);
    zip_error_fini(&error);
    throw_unreadable("", std::move(message));
  }
  zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, &error);
  if (archive == nullptr) {
    std::string message = zip_error_strerror(&error);
    zip_error_fini(&error);
    zip_source_free(source);
    throw_unreadable("", std::move(message));
  }
  zip_error_fini(&error);
  return archive;
}

}  // namespace

PackageReader::EntryStream::EntryStream(zip_file* file, std::string entry,
                                        std::uint64_t size)
    : file_(file), entry_(std::move(entry)), remaining_(size) {}

PackageReader::EntryStream::EntryStream(EntryStream&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)),
      entry_(std::move(other.entry_)),
      remaining_(other.remaining_),
      end_verified_(other.end_verified_) {}

PackageReader::EntryStream::~EntryStream() {
  if (file_ != nullptr) {
    zip_fclose(file_);
  }
}

void PackageReader::EntryStream::verify_end() {
  if (end_verified_) {
    return;
  }
  // The declared size has been read; the entry must have nothing more.
  char probe = 0;
  const auto extra = zip_fread(file_, &probe, 1);
  if (extra < 0) {
    throw_unreadable(entry_, zip_file_strerror(file_));
  }
  if (extra > 0) {
    throw_size_mismatch(entry_,
                        "Package entry holds more data than its declared size.");
  }
  end_verified_ = true;
}

std::size_t PackageReader::EntryStream::read(char* buffer,
                                             std::size_t capacity) {
  if (remaining_ == 0) {
    verify_end();
    return 0;
  }
  const auto wanted = static_cast<zip_uint64_t>(
      std::min<std::uint64_t>(remaining_, capacity));
  const auto count = zip_fread(file_, buffer, wanted);
  if (count < 0) {
    throw_unreadable(entry_, zip_file_strerror(file_));
  }
  if (count == 0) {
    throw_size_mismatch(entry_,
                        "Package entry ended before its declared size.");
  }
  remaining_ -= static_cast<std::uint64_t>(count);
  if (remaining_ == 0) {
    verify_end();
  }
  return static_cast<std::size_t>(count);
}

bool PackageReader::EntryStream::read_exact(std::byte* output,
                                            std::size_t byte_count,
                                            std::string& error_message) {
  std::size_t filled = 0;
  try {
    while (filled < byte_count) {
      const auto count =
          read(reinterpret_cast<char*>(output) + filled, byte_count - filled);
      if (count == 0) {
        error_message = "Package entry ended inside a block.";
        return false;
      }
      filled += count;
    }
  } catch (const ExportError& error) {
    error_message = error.what();
    return false;
  }
  return true;
}

PackageReader::PackageReader(const std::filesystem::path& path,
                             std::optional<PackageByteRange> range)
    : archive_(open_archive(path, range)) {
  const auto entry_count = zip_get_num_entries(archive_, 0);
  if (entry_count < 0) {
    zip_discard(archive_);
    archive_ = nullptr;
    throw_unreadable("", "could not read the ZIP entry count");
  }
  entries_.reserve(static_cast<std::size_t>(entry_count));
  for (zip_int64_t index = 0; index < entry_count; ++index) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive_, static_cast<zip_uint64_t>(index), 0, &stat) !=
            0 ||
        stat.name == nullptr || (stat.valid & ZIP_STAT_SIZE) == 0) {
      const std::string message = zip_strerror(archive_);
      zip_discard(archive_);
      archive_ = nullptr;
      throw_unreadable("", "ZIP entry " + std::to_string(index) +
                               " has no readable name or size: " + message);
    }
    PackageEntry entry;
    entry.name = stat.name;
    entry.zip_index = static_cast<std::uint64_t>(index);
    entry.size_bytes = stat.size;
    entry.is_directory = !entry.name.empty() && entry.name.back() == '/';
    entries_.push_back(std::move(entry));
  }
}

PackageReader::~PackageReader() {
  if (archive_ != nullptr) {
    zip_discard(archive_);
  }
}

const PackageEntry* PackageReader::find_file(std::string_view name) const {
  for (const auto& entry : entries_) {
    if (!entry.is_directory && entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

PackageReader::EntryStream PackageReader::open(
    const PackageEntry& entry) const {
  zip_file_t* file =
      zip_fopen_index(archive_, static_cast<zip_uint64_t>(entry.zip_index), 0);
  if (file == nullptr) {
    throw_unreadable(entry.name, zip_strerror(archive_));
  }
  return EntryStream{file, entry.name, entry.size_bytes};
}

std::string PackageReader::read_whole(const PackageEntry& entry,
                                      std::uint64_t max_bytes) const {
  if (entry.size_bytes > max_bytes) {
    throw ExportError(ExportErrorCode::record_too_large,
                      "Package entry is larger than the export holds in "
                      "memory for one document.",
                      entry_details(entry.name));
  }
  auto stream = open(entry);
  std::string content(static_cast<std::size_t>(entry.size_bytes), '\0');
  std::size_t filled = 0;
  while (filled < content.size()) {
    filled += stream.read(content.data() + filled, content.size() - filled);
  }
  // Confirms the entry ends exactly at its declared size.
  char probe = 0;
  (void)stream.read(&probe, 1);
  return content;
}

}  // namespace package_export
