#include "svp/blocks/block_writer.hpp"
#include "svp/blocks/block_stream.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

using namespace svp::blocks;

void test_round_trip_depth_block() {
  const std::uint32_t width = 4;
  const std::uint32_t height = 2;
  const std::uint64_t frame_count = 1;
  const std::uint64_t uncompressed_size =
      static_cast<std::uint64_t>(width) * height * frame_count * 2;

  std::vector<std::byte> payload(uncompressed_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<std::uint8_t>(i % 256)};
  }

  BlockWriteSpec spec;
  spec.block_type = BlockType::depth;
  spec.extent_0 = width;
  spec.extent_1 = height;
  spec.extent_2 = 1;
  spec.dtype = DType::uint16;
  spec.start_frame = 0;
  spec.frame_count = frame_count;
  spec.start_us = 0;
  spec.end_us = 33333;

  std::vector<std::byte> stream;
  auto info = write_block(stream, spec, payload.data(), payload.size());

  assert(info.block_offset == 0);
  assert(info.payload_offset == kBlockHeaderSize);
  assert(info.block_length == kBlockHeaderSize + info.compressed_size);
  assert(info.uncompressed_size == uncompressed_size);
  assert(info.compressed_size > 0);
  assert(!stream.empty());

  ParseOptions options;
  options.required_block_type = BlockType::depth;
  options.required_raster_extent = RasterExtent{width, height};
  options.verify_hashes = true;
  options.verify_zstd_decompression = true;

  std::size_t read_pos = 0;
  auto result = parse_block_stream(
      stream.size(), options,
      [&](std::byte* output, std::size_t count, std::string& err) -> bool {
        if (read_pos + count > stream.size()) {
          err = "read past end";
          return false;
        }
        std::memcpy(output, stream.data() + read_pos, count);
        read_pos += count;
        return true;
      });

  assert(result.issues.empty());
  assert(result.blocks.size() == 1);
  assert(result.blocks[0].block_type == static_cast<std::uint8_t>(BlockType::depth));
  assert(result.blocks[0].extent_0 == width);
  assert(result.blocks[0].extent_1 == height);
  assert(result.blocks[0].extent_2 == 1);
  assert(result.blocks[0].dtype == static_cast<std::uint32_t>(DType::uint16));
  assert(result.blocks[0].frame_count == frame_count);
  assert(result.blocks[0].start_frame == 0);

  std::cout << "test_round_trip_depth_block: PASS\n";
}

void test_round_trip_embedding_block() {
  const std::uint32_t vector_count = 2;
  const std::uint32_t dimension = 4;
  const std::uint64_t uncompressed_size =
      static_cast<std::uint64_t>(vector_count) * dimension * 4;

  std::vector<std::byte> payload(uncompressed_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<std::uint8_t>(i % 256)};
  }

  BlockWriteSpec spec;
  spec.block_type = BlockType::embedding;
  spec.extent_0 = vector_count;
  spec.extent_1 = dimension;
  spec.extent_2 = 1;
  spec.dtype = DType::float32;
  spec.start_frame = std::numeric_limits<std::uint64_t>::max();
  spec.frame_count = 0;
  spec.start_us = -1;
  spec.end_us = -1;

  std::vector<std::byte> stream;
  auto info = write_block(stream, spec, payload.data(), payload.size());

  assert(info.uncompressed_size == uncompressed_size);
  assert(info.compressed_size > 0);

  ParseOptions options;
  options.required_block_type = BlockType::embedding;
  options.verify_hashes = true;
  options.verify_zstd_decompression = true;

  std::size_t read_pos = 0;
  auto result = parse_block_stream(
      stream.size(), options,
      [&](std::byte* output, std::size_t count, std::string& err) -> bool {
        if (read_pos + count > stream.size()) {
          err = "read past end";
          return false;
        }
        std::memcpy(output, stream.data() + read_pos, count);
        read_pos += count;
        return true;
      });

  assert(result.issues.empty());
  assert(result.blocks.size() == 1);
  assert(result.blocks[0].block_type == static_cast<std::uint8_t>(BlockType::embedding));
  assert(result.blocks[0].extent_0 == vector_count);
  assert(result.blocks[0].extent_1 == dimension);
  assert(result.blocks[0].extent_2 == 1);
  assert(result.blocks[0].dtype == static_cast<std::uint32_t>(DType::float32));
  assert(result.blocks[0].frame_count == 0);

  std::cout << "test_round_trip_embedding_block: PASS\n";
}

void test_multiple_blocks_in_stream() {
  const std::uint32_t width = 2;
  const std::uint32_t height = 2;
  const std::uint64_t frame_count = 1;
  const std::uint64_t payload_size = width * height * frame_count * 2;

  std::vector<std::byte> payload(payload_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<std::uint8_t>(i)};
  }

  BlockWriteSpec spec;
  spec.block_type = BlockType::depth;
  spec.extent_0 = width;
  spec.extent_1 = height;
  spec.extent_2 = 1;
  spec.dtype = DType::uint16;
  spec.start_frame = 0;
  spec.frame_count = frame_count;
  spec.start_us = 0;
  spec.end_us = 33333;

  std::vector<std::byte> stream;
  auto info1 = write_block(stream, spec, payload.data(), payload.size());

  spec.start_frame = 1;
  spec.start_us = 33333;
  spec.end_us = 66666;
  auto info2 = write_block(stream, spec, payload.data(), payload.size());

  assert(info1.block_offset == 0);
  assert(info2.block_offset == info1.block_offset + info1.block_length);

  ParseOptions options;
  options.required_block_type = BlockType::depth;
  options.required_raster_extent = RasterExtent{width, height};

  std::size_t read_pos = 0;
  auto result = parse_block_stream(
      stream.size(), options,
      [&](std::byte* output, std::size_t count, std::string& err) -> bool {
        if (read_pos + count > stream.size()) {
          err = "read past end";
          return false;
        }
        std::memcpy(output, stream.data() + read_pos, count);
        read_pos += count;
        return true;
      });

  assert(result.issues.empty());
  assert(result.blocks.size() == 2);
  assert(result.blocks[0].start_frame == 0);
  assert(result.blocks[1].start_frame == 1);

  std::cout << "test_multiple_blocks_in_stream: PASS\n";
}

void test_stream_writer_matches_vector_writer() {
  const std::uint32_t width = 3;
  const std::uint32_t height = 3;
  const std::uint64_t payload_size = width * height * 2;

  std::vector<std::byte> payload(payload_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<std::uint8_t>((i * 7) % 251)};
  }

  BlockWriteSpec spec;
  spec.block_type = BlockType::depth;
  spec.extent_0 = width;
  spec.extent_1 = height;
  spec.extent_2 = 1;
  spec.dtype = DType::uint16;
  spec.start_frame = 2;
  spec.frame_count = 1;
  spec.start_us = 2000;
  spec.end_us = 3000;

  std::vector<std::byte> vector_stream;
  const auto vector_info =
      write_block(vector_stream, spec, payload.data(), payload.size());

  std::stringstream stream;
  const auto stream_info =
      write_block_to_stream(stream, spec, payload.data(), payload.size());
  const std::string stream_bytes = stream.str();

  assert(stream_info.block_offset == vector_info.block_offset);
  assert(stream_info.block_length == vector_info.block_length);
  assert(stream_info.payload_offset == vector_info.payload_offset);
  assert(stream_info.uncompressed_size == vector_info.uncompressed_size);
  assert(stream_info.compressed_size == vector_info.compressed_size);
  assert(stream_info.payload_blake3 == vector_info.payload_blake3);
  assert(stream_info.header_blake3 == vector_info.header_blake3);
  assert(stream_bytes.size() == vector_stream.size());
  assert(std::memcmp(stream_bytes.data(), vector_stream.data(), vector_stream.size()) == 0);

  std::cout << "test_stream_writer_matches_vector_writer: PASS\n";
}

void test_write_block_to_file() {
  const std::filesystem::path file_path =
      std::filesystem::temp_directory_path() / "svp-block-writer-test.svpdz";

  const std::uint32_t width = 2;
  const std::uint32_t height = 2;
  const std::uint64_t payload_size = width * height * 1 * 2;

  std::vector<std::byte> payload(payload_size);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = std::byte{static_cast<std::uint8_t>(i * 2)};
  }

  BlockWriteSpec spec;
  spec.block_type = BlockType::depth;
  spec.extent_0 = width;
  spec.extent_1 = height;
  spec.extent_2 = 1;
  spec.dtype = DType::uint16;
  spec.start_frame = 0;
  spec.frame_count = 1;
  spec.start_us = 0;
  spec.end_us = 1000;

  auto info = write_block_to_file(file_path, spec, payload.data(), payload.size());
  assert(info.block_offset == 0);
  assert(std::filesystem::exists(file_path));
  assert(std::filesystem::file_size(file_path) == info.block_length);

  std::filesystem::remove(file_path);
  std::cout << "test_write_block_to_file: PASS\n";
}

}  // namespace

int main() {
  test_round_trip_depth_block();
  test_round_trip_embedding_block();
  test_multiple_blocks_in_stream();
  test_stream_writer_matches_vector_writer();
  test_write_block_to_file();
  std::cout << "All svp-blocks writer tests passed.\n";
  return 0;
}
