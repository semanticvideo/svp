#pragma once

#include "svp/blocks/block_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace svp::blocks {

// Decompresses one SVPB block payload (Zstandard, RC2 Section 14.5).
// Throws std::runtime_error when the payload is not a single valid Zstandard
// frame of exactly `uncompressed_size` bytes or exceeds
// kMaxBlockPayloadBytes.
[[nodiscard]] std::vector<std::byte> decompress_block_payload(
    const std::byte* compressed_payload,
    std::uint64_t compressed_size,
    std::uint64_t uncompressed_size);

}  // namespace svp::blocks
