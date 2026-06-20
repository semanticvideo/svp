#pragma once

#include "svp/blocks/block_stream.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace svp::blocks {

struct BlockWriteSpec {
  BlockType block_type = BlockType::depth;
  std::uint32_t extent_0 = 0;
  std::uint32_t extent_1 = 0;
  std::uint32_t extent_2 = 0;
  DType dtype = DType::uint16;
  std::uint64_t start_frame = 0;
  std::uint64_t frame_count = 0;
  std::int64_t start_us = -1;
  std::int64_t end_us = -1;
};

struct WrittenBlockInfo {
  std::uint64_t block_offset = 0;
  std::uint64_t block_length = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t compressed_size = 0;
  std::array<std::uint8_t, 32> payload_blake3{};
  std::array<std::uint8_t, 32> header_blake3{};
};

[[nodiscard]] WrittenBlockInfo write_block(
    std::vector<std::byte>& output_stream,
    const BlockWriteSpec& spec,
    const std::byte* uncompressed_payload,
    std::uint64_t uncompressed_size);

[[nodiscard]] WrittenBlockInfo write_block_to_file(
    const std::filesystem::path& file_path,
    const BlockWriteSpec& spec,
    const std::byte* uncompressed_payload,
    std::uint64_t uncompressed_size);

[[nodiscard]] std::array<std::uint8_t, 32> compute_payload_blake3(
    const std::byte* compressed_payload,
    std::uint64_t compressed_size);

[[nodiscard]] std::array<std::uint8_t, 32> compute_header_blake3(
    const std::array<std::byte, kBlockHeaderSize>& header_bytes);

[[nodiscard]] std::vector<std::byte> zstd_compress(
    const std::byte* input,
    std::uint64_t input_size);

}  // namespace svp::blocks
