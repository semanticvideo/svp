#include "equivalence/package_archive.hpp"

#include <blake3.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace svp::validation::equivalence {
namespace {

// I/O buffer size only; it does not affect comparison results.
constexpr std::size_t kReadChunkBytes = 1U << 20U;
constexpr std::string_view kBlake3Prefix = "blake3:";

struct ZipFileCloser {
  void operator()(zip_file_t* file) const noexcept {
    if (file != nullptr) {
      zip_fclose(file);
    }
  }
};

using ZipFile = std::unique_ptr<zip_file_t, ZipFileCloser>;

ZipFile open_entry(zip_t* archive, const std::string& entry) {
  ZipFile file{zip_fopen(archive, entry.c_str(), 0)};
  if (!file) {
    throw std::runtime_error("could not open package entry " + entry + ": " +
                             zip_strerror(archive));
  }
  return file;
}

std::size_t read_chunk(zip_file_t* file, char* buffer, std::size_t size) {
  std::size_t filled = 0;
  while (filled < size) {
    const auto count = zip_fread(file, buffer + filled, size - filled);
    if (count < 0) {
      throw std::runtime_error(zip_file_strerror(file));
    }
    if (count == 0) {
      break;
    }
    filled += static_cast<std::size_t>(count);
  }
  return filled;
}

}  // namespace

PackageArchive::PackageArchive(const std::filesystem::path& path) {
  int error_code = ZIP_ER_OK;
  archive_ = zip_open(path.string().c_str(), ZIP_RDONLY, &error_code);
  if (archive_ == nullptr) {
    zip_error_t error;
    zip_error_init_with_code(&error, error_code);
    const std::string message = zip_error_strerror(&error);
    zip_error_fini(&error);
    throw std::runtime_error("could not open package " + path.string() + ": " + message);
  }
}

PackageArchive::~PackageArchive() {
  if (archive_ != nullptr) {
    zip_discard(archive_);
  }
}

std::string PackageArchive::read_entry(const std::string& entry) const {
  auto file = open_entry(archive_, entry);
  std::string content;
  std::vector<char> buffer(kReadChunkBytes);
  while (true) {
    const auto count = read_chunk(file.get(), buffer.data(), buffer.size());
    content.append(buffer.data(), count);
    if (count < buffer.size()) {
      return content;
    }
  }
}

std::optional<std::uint64_t> PackageArchive::first_difference(const PackageArchive& left,
                                                              const PackageArchive& right,
                                                              const std::string& entry) {
  auto left_file = open_entry(left.archive_, entry);
  auto right_file = open_entry(right.archive_, entry);
  std::vector<char> left_buffer(kReadChunkBytes);
  std::vector<char> right_buffer(kReadChunkBytes);
  std::uint64_t offset = 0;
  while (true) {
    const auto left_count = read_chunk(left_file.get(), left_buffer.data(), left_buffer.size());
    const auto right_count =
        read_chunk(right_file.get(), right_buffer.data(), right_buffer.size());
    const auto shared = std::min(left_count, right_count);
    const auto mismatch = std::mismatch(left_buffer.begin(),
                                        left_buffer.begin() + static_cast<std::ptrdiff_t>(shared),
                                        right_buffer.begin());
    if (mismatch.first != left_buffer.begin() + static_cast<std::ptrdiff_t>(shared)) {
      return offset + static_cast<std::uint64_t>(mismatch.first - left_buffer.begin());
    }
    if (left_count != right_count) {
      return offset + shared;
    }
    if (left_count == 0) {
      return std::nullopt;
    }
    offset += left_count;
  }
}

FileIdentity file_identity(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw std::runtime_error("could not open " + path.string());
  }
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  FileIdentity identity;
  std::vector<char> buffer(kReadChunkBytes);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    blake3_hasher_update(&hasher, buffer.data(), count);
    identity.size_bytes += count;
  }
  std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  std::ostringstream hex;
  hex << kBlake3Prefix << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    hex << std::setw(2) << static_cast<unsigned int>(byte);
  }
  identity.blake3 = hex.str();
  return identity;
}

}  // namespace svp::validation::equivalence
