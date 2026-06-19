#include "svp/models/hash.hpp"

#include "svp/models/error.hpp"

#include <array>
#include <blake3.h>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace svp::models {
namespace {

std::string to_lower_hex(const std::array<uint8_t, BLAKE3_OUT_LEN>& digest) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return stream.str();
}

}  // namespace

std::string blake3_hex_for_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw ModelError(ModelErrorCode::io_error,
                     "could not open file for BLAKE3 verification: " + path.string());
  }

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = file.gcount();
    if (count > 0) {
      blake3_hasher_update(&hasher, buffer.data(), static_cast<size_t>(count));
    }
  }

  if (file.bad()) {
    throw ModelError(ModelErrorCode::io_error,
                     "failed while reading file for BLAKE3 verification: " +
                         path.string());
  }

  std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return to_lower_hex(digest);
}

}  // namespace svp::models
