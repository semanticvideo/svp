#include "block_stream_export.hpp"

#include "export_error.hpp"

#include "svp/blocks/block_payload.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <string_view>

namespace package_export {
namespace {

std::string lowercase_hex(const std::array<std::uint8_t, 32>& digest) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    text.push_back(kDigits[byte >> 4U]);
    text.push_back(kDigits[byte & 0x0fU]);
  }
  return text;
}

nlohmann::json name_or_null(std::string_view name) {
  return name.empty() ? nlohmann::json(nullptr) : nlohmann::json(name);
}

nlohmann::json block_table_record(const DecodedBlock& block,
                                  const std::string& decoded_file) {
  const auto& header = block.header;
  return nlohmann::json{
      {"block_ordinal", block.ordinal},
      {"block_offset", header.offset},
      {"block_length", block.block_length},
      {"version", header.version},
      {"header_size", header.header_size},
      {"block_type", header.block_type},
      {"block_type_name", name_or_null(block_type_name(header.block_type))},
      {"compression", header.compression},
      {"endian", header.endian},
      {"flags", header.flags},
      {"uncompressed_size", header.uncompressed_size},
      {"compressed_size", header.compressed_size},
      {"extent_0", header.extent_0},
      {"extent_1", header.extent_1},
      {"extent_2", header.extent_2},
      {"dtype", header.dtype},
      {"dtype_name", name_or_null(dtype_name(header.dtype))},
      {"start_frame", header.start_frame},
      {"frame_count", header.frame_count},
      {"start_us", header.start_us},
      {"end_us", header.end_us},
      {"payload_blake3", lowercase_hex(header.payload_blake3)},
      {"header_blake3", lowercase_hex(header.header_blake3)},
      {"decoded_file", decoded_file},
      {"decoded_offset", block.decoded_offset},
      {"decoded_length", header.uncompressed_size},
  };
}

[[noreturn]] void throw_invalid_stream(std::string_view entry,
                                       std::uint64_t offset,
                                       std::string message) {
  throw ExportError(ExportErrorCode::invalid_block_stream,
                    "SVPB block stream failed verification: " + message,
                    nlohmann::json{{"entry", std::string{entry}},
                                   {"offset", offset}});
}

svp::blocks::ParseResult verify_stream(const PackageReader& reader,
                                       const PackageEntry& entry) {
  svp::blocks::ParseOptions options;
  options.verify_hashes = true;
  options.verify_zstd_decompression = true;
  // An empty stream is a stream with no blocks; whether the package needed
  // blocks there is the validator's decision, not the exporter's.
  options.allow_empty = true;
  auto stream = reader.open(entry);
  return svp::blocks::parse_block_stream(
      entry.size_bytes, options,
      [&](std::byte* output, std::size_t byte_count, std::string& error) {
        return stream.read_exact(output, byte_count, error);
      });
}

void read_or_throw(PackageReader::EntryStream& stream, std::byte* output,
                   std::size_t byte_count, std::string_view entry,
                   std::uint64_t offset) {
  std::string error;
  if (!stream.read_exact(output, byte_count, error)) {
    throw_invalid_stream(entry, offset, error);
  }
}

}  // namespace

std::string_view block_type_name(std::uint8_t block_type) noexcept {
  switch (block_type) {
    case static_cast<std::uint8_t>(svp::blocks::BlockType::depth):
      return "depth";
    case static_cast<std::uint8_t>(svp::blocks::BlockType::mask):
      return "mask";
    case static_cast<std::uint8_t>(svp::blocks::BlockType::embedding):
      return "embedding";
    default:
      return {};
  }
}

std::string_view dtype_name(std::uint32_t dtype) noexcept {
  switch (static_cast<svp::blocks::DType>(dtype)) {
    case svp::blocks::DType::uint8:
      return "uint8";
    case svp::blocks::DType::uint16:
      return "uint16";
    case svp::blocks::DType::float16:
      return "float16";
    case svp::blocks::DType::float32:
      return "float32";
    case svp::blocks::DType::bitpacked_lsb_first:
      return "bitpacked_lsb_first";
    case svp::blocks::DType::svp_rle_v1:
      return "svp-rle-v1";
  }
  return {};
}

const DecodedBlock* DecodedBlockStream::find_by_offset(
    std::uint64_t block_offset) const {
  const auto found = std::ranges::lower_bound(
      blocks, block_offset, {},
      [](const DecodedBlock& block) { return block.header.offset; });
  if (found == blocks.end() || found->header.offset != block_offset) {
    return nullptr;
  }
  return &*found;
}

DecodedBlockStream export_block_stream(const PackageReader& reader,
                                       const PlannedLayer& layer,
                                       OutputTransaction& output,
                                       LayerResult& result) {
  const auto& entry = layer.entry;
  const auto parsed = verify_stream(reader, entry);
  if (!parsed.issues.empty()) {
    const auto& issue = parsed.issues.front();
    throw_invalid_stream(entry.name, issue.offset, issue.message);
  }

  const auto& table_file = layer.files.at(0);
  const auto& decoded_file = layer.files.at(1);
  DecodedBlockStream decoded;
  decoded.entry = entry.name;
  decoded.decoded_file = decoded_file.path;
  decoded.blocks.reserve(parsed.blocks.size());

  auto table = output.create_file(table_file.path);
  auto payloads = output.create_file(decoded_file.path);
  auto stream = reader.open(entry);
  std::array<std::byte, svp::blocks::kBlockHeaderSize> header_bytes{};
  std::vector<std::byte> compressed;
  std::uint64_t decoded_offset = 0;
  for (const auto& header : parsed.blocks) {
    read_or_throw(stream, header_bytes.data(), header_bytes.size(), entry.name,
                  header.offset);
    compressed.resize(static_cast<std::size_t>(header.compressed_size));
    read_or_throw(stream, compressed.data(), compressed.size(), entry.name,
                  header.offset);
    std::vector<std::byte> payload;
    try {
      payload = svp::blocks::decompress_block_payload(
          compressed.data(), header.compressed_size, header.uncompressed_size);
    } catch (const std::exception& error) {
      throw_invalid_stream(entry.name, header.offset, error.what());
    }
    payloads.write(std::string_view{
        reinterpret_cast<const char*>(payload.data()), payload.size()});

    DecodedBlock block{
        .header = header,
        .ordinal = decoded.blocks.size(),
        .block_length = svp::blocks::kBlockHeaderSize + header.compressed_size,
        .decoded_offset = decoded_offset,
    };
    table.write(block_table_record(block, decoded.decoded_file).dump());
    table.write("\n");
    decoded_offset += payload.size();
    decoded.blocks.push_back(block);
  }
  char probe = 0;
  if (stream.read(&probe, 1) != 0) {
    throw_invalid_stream(entry.name, entry.size_bytes - stream.remaining(),
                         "data follows the last verified block");
  }
  table.close();
  payloads.close();

  result.file_sizes = {table.size_bytes(), payloads.size_bytes()};
  result.record_count = decoded.blocks.size();
  return decoded;
}

}  // namespace package_export
