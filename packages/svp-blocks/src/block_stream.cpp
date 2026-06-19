#include "svp/blocks/block_stream.hpp"

#include <blake3.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace svp::blocks {
namespace {

constexpr std::uint8_t kCompressionZstd = 0x01;
constexpr std::uint8_t kEndianLittle = 0x01;
constexpr std::uint64_t kNonFrameStart = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kHashSize = 32;
constexpr std::size_t kPayloadChunkSize = 64 * 1024;

std::uint16_t read_u16_le(const std::array<std::byte, kBlockHeaderSize>& bytes,
                          std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]))
       << 8U));
}

std::uint32_t read_u32_le(const std::array<std::byte, kBlockHeaderSize>& bytes,
                          std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (8U * index);
  }
  return value;
}

std::uint64_t read_u64_le(const std::array<std::byte, kBlockHeaderSize>& bytes,
                          std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (8U * index);
  }
  return value;
}

std::int64_t read_i64_le(const std::array<std::byte, kBlockHeaderSize>& bytes,
                         std::size_t offset) noexcept {
  const auto unsigned_value = read_u64_le(bytes, offset);
  std::int64_t signed_value = 0;
  static_assert(sizeof(signed_value) == sizeof(unsigned_value));
  std::memcpy(&signed_value, &unsigned_value, sizeof(signed_value));
  return signed_value;
}

std::array<std::uint8_t, kHashSize> read_hash(
    const std::array<std::byte, kBlockHeaderSize>& bytes,
    std::size_t offset) noexcept {
  std::array<std::uint8_t, kHashSize> value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
  }
  return value;
}

bool checked_multiply(std::uint64_t lhs,
                      std::uint64_t rhs,
                      std::uint64_t& result) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return false;
  }
  result = lhs * rhs;
  return true;
}

bool checked_add(std::uint64_t lhs,
                 std::uint64_t rhs,
                 std::uint64_t& result) noexcept {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return false;
  }
  result = lhs + rhs;
  return true;
}

void add_issue(ParseResult& result,
               IssueKind kind,
               std::uint64_t offset,
               std::string message) {
  result.issues.push_back(BlockStreamIssue{
      .kind = kind,
      .offset = offset,
      .message = std::move(message),
  });
}

BlockHeaderV1 parse_header(const std::array<std::byte, kBlockHeaderSize>& bytes,
                           std::uint64_t offset) noexcept {
  return BlockHeaderV1{
      .offset = offset,
      .version = read_u16_le(bytes, 4),
      .header_size = read_u16_le(bytes, 6),
      .block_type = std::to_integer<std::uint8_t>(bytes[8]),
      .compression = std::to_integer<std::uint8_t>(bytes[9]),
      .endian = std::to_integer<std::uint8_t>(bytes[10]),
      .flags = std::to_integer<std::uint8_t>(bytes[11]),
      .uncompressed_size = read_u64_le(bytes, 12),
      .compressed_size = read_u64_le(bytes, 20),
      .extent_0 = read_u32_le(bytes, 28),
      .extent_1 = read_u32_le(bytes, 32),
      .extent_2 = read_u32_le(bytes, 36),
      .dtype = read_u32_le(bytes, 40),
      .start_frame = read_u64_le(bytes, 44),
      .frame_count = read_u64_le(bytes, 52),
      .start_us = read_i64_le(bytes, 60),
      .end_us = read_i64_le(bytes, 68),
      .payload_blake3 = read_hash(bytes, 76),
      .header_blake3 = read_hash(bytes, 108),
  };
}

bool is_header_magic_valid(const std::array<std::byte, kBlockHeaderSize>& bytes) noexcept {
  return std::to_integer<char>(bytes[0]) == 'S' &&
         std::to_integer<char>(bytes[1]) == 'V' &&
         std::to_integer<char>(bytes[2]) == 'P' &&
         std::to_integer<char>(bytes[3]) == 'B';
}

bool reserved_bytes_are_zero(const std::array<std::byte, kBlockHeaderSize>& bytes) noexcept {
  for (std::size_t index = 140; index < 160; ++index) {
    if (std::to_integer<std::uint8_t>(bytes[index]) != 0) {
      return false;
    }
  }
  return true;
}

std::array<std::uint8_t, kHashSize> blake3_digest_for_header(
    std::array<std::byte, kBlockHeaderSize> bytes) noexcept {
  for (std::size_t index = 108; index < 140; ++index) {
    bytes[index] = std::byte{0};
  }

  std::array<std::uint8_t, kHashSize> digest{};
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return digest;
}

bool is_strict_block_type(std::uint8_t block_type) noexcept {
  return block_type == static_cast<std::uint8_t>(BlockType::depth) ||
         block_type == static_cast<std::uint8_t>(BlockType::mask) ||
         block_type == static_cast<std::uint8_t>(BlockType::embedding);
}

bool is_time_range_valid(const BlockHeaderV1& header) noexcept {
  if (header.start_us == -1 && header.end_us == -1) {
    return true;
  }
  return header.start_us >= 0 && header.end_us > header.start_us;
}

bool validate_uncompressed_size(const BlockHeaderV1& header, std::string& message) {
  std::uint64_t pixels = 0;
  std::uint64_t samples = 0;
  std::uint64_t expected = 0;

  if (header.block_type == static_cast<std::uint8_t>(BlockType::depth)) {
    if (!checked_multiply(header.extent_0, header.extent_1, pixels) ||
        !checked_multiply(pixels, header.frame_count, samples) ||
        !checked_multiply(samples, 2, expected)) {
      message = "Depth block dimensions overflow payload-size calculation.";
      return false;
    }
    if (header.uncompressed_size != expected) {
      message = "Depth block uncompressed_size does not match dimensions and frame_count.";
      return false;
    }
    return true;
  }

  if (header.block_type == static_cast<std::uint8_t>(BlockType::embedding)) {
    if (!checked_multiply(header.extent_0, header.extent_1, samples) ||
        !checked_multiply(samples, 4, expected)) {
      message = "Embedding block dimensions overflow payload-size calculation.";
      return false;
    }
    if (header.uncompressed_size != expected) {
      message = "Embedding block uncompressed_size does not match vector count and dimension.";
      return false;
    }
    return true;
  }

  if (header.block_type == static_cast<std::uint8_t>(BlockType::mask) &&
      header.dtype == static_cast<std::uint32_t>(DType::bitpacked_lsb_first)) {
    if (!checked_multiply(header.extent_0, header.extent_1, pixels) ||
        !checked_multiply(pixels, header.extent_2, samples)) {
      message = "Mask block dimensions overflow payload-size calculation.";
      return false;
    }
    expected = (samples + 7) / 8;
    if (header.uncompressed_size != expected) {
      message = "Bitpacked mask uncompressed_size does not match dimensions.";
      return false;
    }
  }

  return true;
}

void validate_header(ParseResult& result,
                     const std::array<std::byte, kBlockHeaderSize>& bytes,
                     const BlockHeaderV1& header,
                     const ParseOptions& options) {
  if (!is_header_magic_valid(bytes)) {
    add_issue(result, IssueKind::invalid_header, header.offset, "Block magic must be SVPB.");
  }
  if (header.version != 1) {
    add_issue(result, IssueKind::invalid_header, header.offset, "Block version must be 1.");
  }
  if (header.header_size != kBlockHeaderSize) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block header_size must be 160.");
  }
  if (header.compression != kCompressionZstd) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block compression must be Zstandard code 0x01.");
  }
  if (header.endian != kEndianLittle) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block endian must be little-endian code 0x01.");
  }
  if (header.flags != 0) {
    add_issue(result, IssueKind::invalid_header, header.offset, "Block flags must be zero.");
  }
  if (!reserved_bytes_are_zero(bytes)) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block reserved bytes must be zero.");
  }

  if (!is_strict_block_type(header.block_type)) {
    add_issue(result, IssueKind::forbidden_block_type, header.offset,
              "Block type is not allowed in strict v1.0 packages.");
    return;
  }

  if (options.required_block_type.has_value() &&
      header.block_type != static_cast<std::uint8_t>(*options.required_block_type)) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block type does not match the package stream path.");
  }

  if (header.compressed_size == 0 || header.uncompressed_size == 0) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block payload sizes must be nonzero.");
  }

  if (header.compressed_size > options.max_compressed_payload_bytes) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block compressed payload exceeds validator size guard.");
  }

  if (header.uncompressed_size > options.max_uncompressed_payload_bytes) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block uncompressed payload exceeds validator size guard.");
  }

  if (!is_time_range_valid(header)) {
    add_issue(result, IssueKind::invalid_header, header.offset,
              "Block time range must be absent or have end_us greater than start_us.");
  }

  std::string size_message;
  if (!validate_uncompressed_size(header, size_message)) {
    add_issue(result, IssueKind::invalid_header, header.offset, std::move(size_message));
  }

  if (header.block_type == static_cast<std::uint8_t>(BlockType::depth)) {
    if (header.extent_0 == 0 || header.extent_1 == 0 || header.extent_2 != 1 ||
        header.dtype != static_cast<std::uint32_t>(DType::uint16) ||
        header.frame_count == 0 || header.start_frame == kNonFrameStart) {
      add_issue(result, IssueKind::invalid_header, header.offset,
                "Depth blocks require uint16 image extents, one plane, and a frame range.");
    }
  } else if (header.block_type == static_cast<std::uint8_t>(BlockType::mask)) {
    const auto dtype = static_cast<DType>(header.dtype);
    if (header.extent_0 == 0 || header.extent_1 == 0 || header.extent_2 == 0 ||
        (dtype != DType::bitpacked_lsb_first && dtype != DType::svp_rle_v1) ||
        header.frame_count == 0 || header.start_frame == kNonFrameStart) {
      add_issue(result, IssueKind::invalid_header, header.offset,
                "Mask blocks require mask extents, mask dtype, and a frame range.");
    }
  } else if (header.block_type == static_cast<std::uint8_t>(BlockType::embedding)) {
    if (header.extent_0 == 0 || header.extent_1 == 0 || header.extent_2 != 1 ||
        header.dtype != static_cast<std::uint32_t>(DType::float32) ||
        header.frame_count != 0 || header.start_frame != kNonFrameStart) {
      add_issue(result, IssueKind::invalid_header, header.offset,
                "Embedding blocks require float32 vectors and a non-frame range.");
    }
  }

  if (options.required_raster_extent.has_value() &&
      (header.block_type == static_cast<std::uint8_t>(BlockType::depth) ||
       header.block_type == static_cast<std::uint8_t>(BlockType::mask)) &&
      (header.extent_0 != options.required_raster_extent->width ||
       header.extent_1 != options.required_raster_extent->height)) {
    add_issue(result, IssueKind::raster_extent_mismatch, header.offset,
              "Spatial block extents must match manifest canonical_analysis_raster.");
  }
}

struct ZstdDStreamDeleter {
  void operator()(ZSTD_DStream* stream) const noexcept {
    ZSTD_freeDStream(stream);
  }
};

bool validate_payload(ParseResult& result,
                      const ReadExact& read_exact,
                      const BlockHeaderV1& header,
                      const ParseOptions& options,
                      std::uint64_t payload_offset,
                      std::string& error_message) {
  std::array<std::byte, kPayloadChunkSize> input_buffer{};
  std::array<std::byte, kPayloadChunkSize> output_buffer{};
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  std::unique_ptr<ZSTD_DStream, ZstdDStreamDeleter> zstd_stream;
  if (options.verify_zstd_decompression) {
    zstd_stream.reset(ZSTD_createDStream());
    if (!zstd_stream) {
      add_issue(result, IssueKind::decompression_failed, payload_offset,
                "Could not allocate Zstandard decompression stream.");
    } else {
      const auto init_result = ZSTD_initDStream(zstd_stream.get());
      if (ZSTD_isError(init_result) != 0U) {
        add_issue(result, IssueKind::decompression_failed, payload_offset,
                  std::string{"Could not initialize Zstandard decompression: "} +
                      ZSTD_getErrorName(init_result));
        zstd_stream.reset();
      }
    }
  }

  bool zstd_failed = false;
  bool zstd_frame_complete = !options.verify_zstd_decompression || !zstd_stream;
  std::uint64_t decompressed_size = 0;
  auto remaining = header.compressed_size;
  while (remaining > 0) {
    const auto chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, input_buffer.size()));
    if (!read_exact(input_buffer.data(), chunk, error_message)) {
      return false;
    }
    blake3_hasher_update(&hasher, input_buffer.data(), chunk);

    if (zstd_stream && !zstd_failed) {
      ZSTD_inBuffer input{
          .src = input_buffer.data(),
          .size = chunk,
          .pos = 0,
      };
      while (input.pos < input.size) {
        ZSTD_outBuffer output{
            .dst = output_buffer.data(),
            .size = output_buffer.size(),
            .pos = 0,
        };
        const auto decompress_result =
            ZSTD_decompressStream(zstd_stream.get(), &output, &input);
        if (ZSTD_isError(decompress_result) != 0U) {
          add_issue(result, IssueKind::decompression_failed, payload_offset,
                    std::string{"Block compressed payload is not valid Zstandard: "} +
                        ZSTD_getErrorName(decompress_result));
          zstd_failed = true;
          break;
        }
        if (output.pos > std::numeric_limits<std::uint64_t>::max() -
                             decompressed_size) {
          add_issue(result, IssueKind::decompression_failed, payload_offset,
                    "Block decompressed payload size overflowed.");
          zstd_failed = true;
          break;
        }
        decompressed_size += static_cast<std::uint64_t>(output.pos);
        if (decompressed_size > header.uncompressed_size) {
          add_issue(result, IssueKind::decompression_failed, payload_offset,
                    "Block decompressed payload exceeds declared uncompressed_size.");
          zstd_failed = true;
          break;
        }
        zstd_frame_complete = decompress_result == 0;
      }
    }

    remaining -= chunk;
  }

  std::array<std::uint8_t, kHashSize> payload_digest{};
  blake3_hasher_finalize(&hasher, payload_digest.data(), payload_digest.size());
  if (options.verify_hashes && payload_digest != header.payload_blake3) {
    add_issue(result, IssueKind::invalid_hash, payload_offset,
              "Block payload_blake3 does not match compressed payload bytes.");
  }

  if (zstd_stream && !zstd_failed) {
    if (!zstd_frame_complete) {
      add_issue(result, IssueKind::decompression_failed, payload_offset,
                "Block compressed payload ended before the Zstandard frame completed.");
    }
    if (decompressed_size != header.uncompressed_size) {
      add_issue(result, IssueKind::decompression_failed, payload_offset,
                "Block decompressed payload size does not match uncompressed_size.");
    }
  }

  return true;
}

}  // namespace

ParseResult parse_block_stream(std::uint64_t stream_size,
                               const ParseOptions& options,
                               const ReadExact& read_exact) {
  ParseResult result;
  if (stream_size == 0) {
    if (!options.allow_empty) {
      add_issue(result, IssueKind::invalid_header, 0, "Block stream must not be empty.");
    }
    return result;
  }

  std::uint64_t offset = 0;
  while (offset < stream_size) {
    if (stream_size - offset < kBlockHeaderSize) {
      add_issue(result, IssueKind::invalid_header, offset,
                "Block stream ended before a complete header.");
      break;
    }

    std::array<std::byte, kBlockHeaderSize> header_bytes{};
    std::string read_error;
    if (!read_exact(header_bytes.data(), header_bytes.size(), read_error)) {
      add_issue(result, IssueKind::invalid_header, offset, std::move(read_error));
      break;
    }

    auto header = parse_header(header_bytes, offset);
    validate_header(result, header_bytes, header, options);
    if (options.verify_hashes &&
        blake3_digest_for_header(header_bytes) != header.header_blake3) {
      add_issue(result, IssueKind::invalid_hash, offset,
                "Block header_blake3 does not match header bytes.");
    }
    result.blocks.push_back(header);

    std::uint64_t payload_start = 0;
    if (!checked_add(offset, kBlockHeaderSize, payload_start)) {
      add_issue(result, IssueKind::invalid_header, offset, "Block offset overflow.");
      break;
    }

    std::uint64_t next_offset = 0;
    if (!checked_add(payload_start, header.compressed_size, next_offset)) {
      add_issue(result, IssueKind::invalid_header, offset,
                "Block compressed payload range overflows.");
      break;
    }

    if (next_offset > stream_size) {
      add_issue(result, IssueKind::invalid_header, offset,
                "Block compressed payload extends past the stream length.");
      break;
    }

    if (header.compressed_size > options.max_compressed_payload_bytes ||
        header.uncompressed_size > options.max_uncompressed_payload_bytes) {
      break;
    }

    if (!validate_payload(result, read_exact, header, options, payload_start,
                          read_error)) {
      add_issue(result, IssueKind::invalid_header, payload_start, std::move(read_error));
      break;
    }

    offset = next_offset;
  }

  return result;
}

}  // namespace svp::blocks
