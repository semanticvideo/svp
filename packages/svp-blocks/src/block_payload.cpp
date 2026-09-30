#include "svp/blocks/block_payload.hpp"

#include <zstd.h>

#include <stdexcept>
#include <string>

namespace svp::blocks {

std::vector<std::byte> decompress_block_payload(const std::byte* compressed_payload,
                                                std::uint64_t compressed_size,
                                                std::uint64_t uncompressed_size) {
  if (uncompressed_size > kMaxBlockPayloadBytes ||
      compressed_size > kMaxBlockPayloadBytes) {
    throw std::runtime_error("Block payload exceeds the SVPB payload size limit.");
  }

  std::vector<std::byte> output(static_cast<std::size_t>(uncompressed_size));
  const auto result = ZSTD_decompress(output.data(), output.size(), compressed_payload,
                                      static_cast<std::size_t>(compressed_size));
  if (ZSTD_isError(result) != 0U) {
    throw std::runtime_error(std::string{"Block payload is not valid Zstandard: "} +
                             ZSTD_getErrorName(result));
  }
  if (result != output.size()) {
    throw std::runtime_error(
        "Block decompressed payload size does not match uncompressed_size.");
  }
  return output;
}

}  // namespace svp::blocks
