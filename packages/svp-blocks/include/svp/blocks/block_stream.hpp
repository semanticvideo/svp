#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::blocks {

inline constexpr std::size_t kBlockHeaderSize = 160;
inline constexpr std::uint64_t kMaxBlockPayloadBytes = 512ULL * 1024ULL * 1024ULL;

enum class BlockType : std::uint8_t {
  depth = 0x01,
  mask = 0x02,
  embedding = 0x03,
};

enum class DType : std::uint32_t {
  uint8 = 1,
  uint16 = 2,
  float16 = 3,
  float32 = 4,
  bitpacked_lsb_first = 5,
  svp_rle_v1 = 6,
};

enum class IssueKind {
  invalid_header,
  invalid_hash,
  decompression_failed,
  forbidden_block_type,
  raster_extent_mismatch,
};

struct RasterExtent {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct BlockHeaderV1 {
  std::uint64_t offset = 0;
  std::uint16_t version = 0;
  std::uint16_t header_size = 0;
  std::uint8_t block_type = 0;
  std::uint8_t compression = 0;
  std::uint8_t endian = 0;
  std::uint8_t flags = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t compressed_size = 0;
  std::uint32_t extent_0 = 0;
  std::uint32_t extent_1 = 0;
  std::uint32_t extent_2 = 0;
  std::uint32_t dtype = 0;
  std::uint64_t start_frame = 0;
  std::uint64_t frame_count = 0;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  std::array<std::uint8_t, 32> payload_blake3{};
  std::array<std::uint8_t, 32> header_blake3{};
};

struct BlockStreamIssue {
  IssueKind kind = IssueKind::invalid_header;
  std::uint64_t offset = 0;
  std::string message;
};

struct ParseOptions {
  std::optional<BlockType> required_block_type;
  std::optional<RasterExtent> required_raster_extent;
  std::uint64_t max_compressed_payload_bytes = kMaxBlockPayloadBytes;
  std::uint64_t max_uncompressed_payload_bytes = kMaxBlockPayloadBytes;
  bool verify_hashes = true;
  bool verify_zstd_decompression = true;
  bool allow_empty = false;
};

struct ParseResult {
  std::vector<BlockHeaderV1> blocks;
  std::vector<BlockStreamIssue> issues;
};

using ReadExact = std::function<bool(std::byte* output,
                                     std::size_t byte_count,
                                     std::string& error_message)>;

[[nodiscard]] ParseResult parse_block_stream(std::uint64_t stream_size,
                                             const ParseOptions& options,
                                             const ReadExact& read_exact);

}  // namespace svp::blocks
