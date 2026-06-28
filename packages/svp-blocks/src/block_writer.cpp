#include "svp/blocks/block_writer.hpp"

#include <blake3.h>
#include <zstd.h>

#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace svp::blocks {
namespace {

constexpr std::uint8_t kCompressionZstd = 0x01;
constexpr std::uint8_t kEndianLittle = 0x01;

void write_u16_le(std::array<std::byte, kBlockHeaderSize>& bytes,
                  std::size_t offset,
                  std::uint16_t value) noexcept {
  bytes[offset] = std::byte{static_cast<std::uint8_t>(value & 0xFFU)};
  bytes[offset + 1] = std::byte{static_cast<std::uint8_t>((value >> 8U) & 0xFFU)};
}

void write_u32_le(std::array<std::byte, kBlockHeaderSize>& bytes,
                  std::size_t offset,
                  std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = std::byte{static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU)};
  }
}

void write_u64_le(std::array<std::byte, kBlockHeaderSize>& bytes,
                  std::size_t offset,
                  std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[offset + i] = std::byte{static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU)};
  }
}

void write_i64_le(std::array<std::byte, kBlockHeaderSize>& bytes,
                  std::size_t offset,
                  std::int64_t value) noexcept {
  std::uint64_t uval = 0;
  std::memcpy(&uval, &value, sizeof(uval));
  write_u64_le(bytes, offset, uval);
}

void write_hash(std::array<std::byte, kBlockHeaderSize>& bytes,
                std::size_t offset,
                const std::array<std::uint8_t, 32>& hash) noexcept {
  for (std::size_t i = 0; i < hash.size(); ++i) {
    bytes[offset + i] = std::byte{hash[i]};
  }
}

std::array<std::byte, kBlockHeaderSize> make_block_header(
    const BlockWriteSpec& spec,
    std::uint64_t uncompressed_size,
    std::uint64_t compressed_size,
    const std::array<std::uint8_t, 32>& payload_hash,
    std::array<std::uint8_t, 32>& header_hash) {
  std::array<std::byte, kBlockHeaderSize> header{};
  header[0] = std::byte{'S'};
  header[1] = std::byte{'V'};
  header[2] = std::byte{'P'};
  header[3] = std::byte{'B'};
  write_u16_le(header, 4, 1);
  write_u16_le(header, 6, static_cast<std::uint16_t>(kBlockHeaderSize));
  header[8] = std::byte{static_cast<std::uint8_t>(spec.block_type)};
  header[9] = std::byte{kCompressionZstd};
  header[10] = std::byte{kEndianLittle};
  header[11] = std::byte{0};
  write_u64_le(header, 12, uncompressed_size);
  write_u64_le(header, 20, compressed_size);
  write_u32_le(header, 28, spec.extent_0);
  write_u32_le(header, 32, spec.extent_1);
  write_u32_le(header, 36, spec.extent_2);
  write_u32_le(header, 40, static_cast<std::uint32_t>(spec.dtype));
  write_u64_le(header, 44, spec.start_frame);
  write_u64_le(header, 52, spec.frame_count);
  write_i64_le(header, 60, spec.start_us);
  write_i64_le(header, 68, spec.end_us);
  write_hash(header, 76, payload_hash);

  header_hash = compute_header_blake3(header);
  write_hash(header, 108, header_hash);
  return header;
}

WrittenBlockInfo make_block_info(
    std::uint64_t block_offset,
    std::uint64_t compressed_size,
    std::uint64_t uncompressed_size,
    const std::array<std::uint8_t, 32>& payload_hash,
    const std::array<std::uint8_t, 32>& header_hash) {
  WrittenBlockInfo info;
  info.block_offset = block_offset;
  info.block_length = kBlockHeaderSize + compressed_size;
  info.payload_offset = block_offset + kBlockHeaderSize;
  info.uncompressed_size = uncompressed_size;
  info.compressed_size = compressed_size;
  info.payload_blake3 = payload_hash;
  info.header_blake3 = header_hash;
  return info;
}

}  // namespace

std::array<std::uint8_t, 32> compute_payload_blake3(
    const std::byte* compressed_payload,
    std::uint64_t compressed_size) {
  std::array<std::uint8_t, 32> digest{};
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, compressed_payload, compressed_size);
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return digest;
}

std::array<std::uint8_t, 32> compute_header_blake3(
    const std::array<std::byte, kBlockHeaderSize>& header_bytes) {
  std::array<std::byte, kBlockHeaderSize> bytes = header_bytes;
  for (std::size_t i = 108; i < 140; ++i) {
    bytes[i] = std::byte{0};
  }
  std::array<std::uint8_t, 32> digest{};
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return digest;
}

std::vector<std::byte> zstd_compress(
    const std::byte* input,
    std::uint64_t input_size) {
  const auto bound = ZSTD_compressBound(static_cast<std::size_t>(input_size));
  if (ZSTD_isError(bound) != 0U) {
    throw std::runtime_error("ZSTD_compressBound failed");
  }
  std::vector<std::byte> output(bound);
  const auto result = ZSTD_compress(
      output.data(), output.size(),
      input, static_cast<std::size_t>(input_size),
      ZSTD_CLEVEL_DEFAULT);
  if (ZSTD_isError(result) != 0U) {
    throw std::runtime_error(std::string{"ZSTD_compress failed: "} +
                             ZSTD_getErrorName(result));
  }
  output.resize(result);
  return output;
}

WrittenBlockInfo write_block(
    std::vector<std::byte>& output_stream,
    const BlockWriteSpec& spec,
    const std::byte* uncompressed_payload,
    std::uint64_t uncompressed_size) {
  if (uncompressed_size == 0) {
    throw std::runtime_error("Block payload must be nonzero.");
  }

  auto compressed = zstd_compress(uncompressed_payload, uncompressed_size);
  if (compressed.empty()) {
    throw std::runtime_error("Zstandard compression produced an empty frame.");
  }

  const auto payload_hash = compute_payload_blake3(compressed.data(), compressed.size());

  std::array<std::uint8_t, 32> header_hash{};
  const auto header = make_block_header(
      spec, uncompressed_size, compressed.size(), payload_hash, header_hash);

  const std::uint64_t block_offset = output_stream.size();

  output_stream.insert(output_stream.end(), header.begin(), header.end());
  output_stream.insert(output_stream.end(), compressed.begin(), compressed.end());

  return make_block_info(
      block_offset, compressed.size(), uncompressed_size, payload_hash, header_hash);
}

WrittenBlockInfo write_block_to_file(
    const std::filesystem::path& file_path,
    const BlockWriteSpec& spec,
    const std::byte* uncompressed_payload,
    std::uint64_t uncompressed_size) {
  std::filesystem::create_directories(file_path.parent_path());
  std::ofstream out(file_path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Unable to open block stream file: " + file_path.string());
  }
  return write_block_to_stream(out, spec, uncompressed_payload, uncompressed_size);
}

WrittenBlockInfo write_block_to_stream(
    std::ostream& output_stream,
    const BlockWriteSpec& spec,
    const std::byte* uncompressed_payload,
    std::uint64_t uncompressed_size) {
  if (uncompressed_size == 0) {
    throw std::runtime_error("Block payload must be nonzero.");
  }

  const auto position = output_stream.tellp();
  if (position < std::streampos{0}) {
    throw std::runtime_error("Unable to determine block stream write offset.");
  }
  const auto position_u64 = static_cast<std::uint64_t>(position);
  if (position_u64 > std::numeric_limits<std::uint64_t>::max() - kBlockHeaderSize) {
    throw std::runtime_error("Block stream offset overflow.");
  }

  auto compressed = zstd_compress(uncompressed_payload, uncompressed_size);
  if (compressed.empty()) {
    throw std::runtime_error("Zstandard compression produced an empty frame.");
  }

  const auto payload_hash = compute_payload_blake3(compressed.data(), compressed.size());
  std::array<std::uint8_t, 32> header_hash{};
  const auto header = make_block_header(
      spec, uncompressed_size, compressed.size(), payload_hash, header_hash);

  output_stream.write(reinterpret_cast<const char*>(header.data()),
                      static_cast<std::streamsize>(header.size()));
  output_stream.write(reinterpret_cast<const char*>(compressed.data()),
                      static_cast<std::streamsize>(compressed.size()));
  if (!output_stream) {
    throw std::runtime_error("Failed to write block stream.");
  }

  return make_block_info(
      position_u64, compressed.size(), uncompressed_size, payload_hash, header_hash);
}

}  // namespace svp::blocks
