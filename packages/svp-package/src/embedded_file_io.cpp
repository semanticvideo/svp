#include "embedded_file_io.hpp"

#include <blake3.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace svp::package::detail {
namespace {

constexpr std::size_t kCopyBufferSize = 1024 * 1024;

}  // namespace

TemporaryOutput::~TemporaryOutput() {
  if (!committed && !path.empty()) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
}

bool make_temporary_output(const std::filesystem::path& output_path,
                           TemporaryOutput& temporary,
                           std::string& error_message) {
  const auto parent = output_path.has_parent_path()
                          ? output_path.parent_path()
                          : std::filesystem::current_path();
  if (!std::filesystem::is_directory(parent)) {
    error_message = "Output directory does not exist.";
    return false;
  }
  auto pattern = (parent / ("." + output_path.filename().string() +
                            ".svp-tmp-XXXXXX")).string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int descriptor = mkstemp(writable.data());
  if (descriptor < 0) {
    error_message = "Unable to create temporary output: " +
                    std::string(std::strerror(errno));
    return false;
  }
  close(descriptor);
  temporary.path = writable.data();
  return true;
}

bool finalize_temporary_output(TemporaryOutput& temporary,
                               const std::filesystem::path& output_path,
                               std::string& error_message) {
  std::error_code error;
  std::filesystem::rename(temporary.path, output_path, error);
  if (error) {
    error_message = "Unable to publish output atomically: " + error.message();
    return false;
  }
  temporary.committed = true;
  return true;
}

bool copy_bytes(std::ifstream& input, std::ofstream& output,
                std::uint64_t offset, std::uint64_t size) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max())) {
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input) {
    return false;
  }
  std::vector<char> buffer(kCopyBufferSize);
  while (size > 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(size, buffer.size()));
    input.read(buffer.data(), chunk);
    if (input.gcount() != chunk) {
      return false;
    }
    output.write(buffer.data(), chunk);
    if (!output) {
      return false;
    }
    size -= static_cast<std::uint64_t>(chunk);
  }
  return true;
}

bool hash_file(const std::filesystem::path& path, std::uint64_t& size,
               std::array<std::uint8_t, 32>& hash) {
  std::error_code error;
  size = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  std::vector<char> buffer(kCopyBufferSize);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (!input.eof()) {
    return false;
  }
  blake3_hasher_finalize(&hasher, hash.data(), hash.size());
  return true;
}

bool output_is_allowed(const std::filesystem::path& output_path, bool overwrite,
                       std::string& error_message) {
  std::error_code error;
  const bool exists = std::filesystem::exists(output_path, error);
  if (error) {
    error_message = "Unable to inspect output path: " + error.message();
    return false;
  }
  if (exists && !overwrite) {
    error_message = "Output already exists; pass the explicit overwrite option to replace it.";
    return false;
  }
  return true;
}

EmbeddedSvpiOperationResult failure_result(
    const std::filesystem::path& output_path,
    EmbeddedSvpiInspection inspection,
    std::string message) {
  return {
      .success = false,
      .output_path = output_path,
      .inspection = std::move(inspection),
      .error_message = std::move(message),
  };
}

}  // namespace svp::package::detail
