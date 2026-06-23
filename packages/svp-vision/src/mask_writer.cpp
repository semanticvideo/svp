#include "svp/vision/mask_writer.hpp"

#include "svp/vision/visual_entity_tracker.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace svp::vision {
namespace {

std::string hash_to_hex(const std::array<std::uint8_t, 32>& hash) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < hash.size(); ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(hash[i]);
  }
  return oss.str();
}

}  // namespace

MaskWriteSummary write_masks(
    const std::filesystem::path& staging_dir,
    const std::vector<MaskWriteEntry>& masks) {
  MaskWriteSummary summary;

  const std::filesystem::path spatial_dir = staging_dir / "spatial";
  std::filesystem::create_directories(spatial_dir);

  const std::filesystem::path index_path = spatial_dir / "masks.index.jsonl";
  const std::filesystem::path block_path = spatial_dir / "masks.blocks.svpdz";

  summary.masks_index_path = index_path.string();
  summary.masks_block_path = block_path.string();
  summary.mask_count = static_cast<int>(masks.size());

  // Write all mask blocks to the block stream file
  std::vector<std::byte> block_stream;

  for (const auto& mask : masks) {
    svp::blocks::BlockWriteSpec spec;
    spec.block_type = svp::blocks::BlockType::mask;
    spec.extent_0 = static_cast<std::uint32_t>(mask.width);
    spec.extent_1 = static_cast<std::uint32_t>(mask.height);
    spec.extent_2 = 1;
    spec.dtype = svp::blocks::DType::svp_rle_v1;
    spec.start_frame = 0;
    spec.frame_count = 1;
    spec.start_us = mask.timestamp_us;
    spec.end_us = mask.timestamp_us;

    auto block_info = svp::blocks::write_block(
        block_stream, spec,
        reinterpret_cast<const std::byte*>(mask.rle_data.data()),
        mask.rle_data.size());

    // Build index record
    nlohmann::json record;
    record["mask_id"] = mask.mask_id;
    record["entity_id"] = mask.entity_id;
    record["track_id"] = mask.track_id;
    record["region_id"] = mask.region_id;
    record["frame_id"] = mask.frame_id;
    record["timestamp_us"] = mask.timestamp_us;
    record["width"] = mask.width;
    record["height"] = mask.height;
    record["encoding"] = "svp_rle_v1";
    record["block_file"] = "spatial/masks.blocks.svpdz";
    record["block_offset"] = block_info.block_offset;
    record["block_length"] = block_info.block_length;
    record["payload_offset"] = block_info.payload_offset;
    record["uncompressed_size"] = block_info.uncompressed_size;
    record["compressed_size"] = block_info.compressed_size;
    record["payload_blake3"] = "blake3:" + hash_to_hex(block_info.payload_blake3);
    record["header_blake3"] = "blake3:" + hash_to_hex(block_info.header_blake3);

    // Block manifest entry
    nlohmann::json block_entry;
    block_entry["block_id"] = mask.mask_id;
    block_entry["block_type"] = "mask";
    block_entry["block_file"] = "spatial/masks.blocks.svpdz";
    block_entry["block_offset"] = block_info.block_offset;
    block_entry["block_length"] = block_info.block_length;
    block_entry["payload_offset"] = block_info.payload_offset;
    block_entry["uncompressed_size"] = block_info.uncompressed_size;
    block_entry["compressed_size"] = block_info.compressed_size;
    block_entry["extent_0"] = static_cast<std::uint32_t>(mask.width);
    block_entry["extent_1"] = static_cast<std::uint32_t>(mask.height);
    block_entry["extent_2"] = 1;
    block_entry["dtype"] = static_cast<std::uint32_t>(svp::blocks::DType::svp_rle_v1);
    block_entry["start_frame"] = 0;
    block_entry["frame_count"] = 1;
    block_entry["start_us"] = mask.timestamp_us;
    block_entry["end_us"] = mask.timestamp_us;
    block_entry["payload_blake3"] = "blake3:" + hash_to_hex(block_info.payload_blake3);
    block_entry["header_blake3"] = "blake3:" + hash_to_hex(block_info.header_blake3);
    block_entry["block_blake3"] = "blake3:" + hash_to_hex(block_info.payload_blake3);

    summary.index_records.push_back(record);
    summary.block_manifest_entries.push_back(block_entry);
  }

  // Write block stream to file
  if (!block_stream.empty()) {
    std::ofstream block_file(block_path, std::ios::binary);
    block_file.write(reinterpret_cast<const char*>(block_stream.data()),
                     static_cast<std::streamsize>(block_stream.size()));
  }

  // Write index JSONL
  std::ofstream index_file(index_path);
  for (const auto& record : summary.index_records) {
    index_file << record.dump() << "\n";
  }

  return summary;
}

}  // namespace svp::vision
