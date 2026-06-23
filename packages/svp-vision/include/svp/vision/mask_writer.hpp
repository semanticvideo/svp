#pragma once

#include "svp/blocks/block_writer.hpp"
#include "svp/blocks/block_stream.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::vision {

struct MaskWriteEntry {
  std::string mask_id;
  std::string entity_id;
  std::string track_id;
  std::string region_id;
  std::string frame_id;
  std::int64_t timestamp_us = 0;
  int width = 0;
  int height = 0;
  // RLE-encoded mask data (SVP RLE v1 format per spec §14.3)
  std::vector<std::uint8_t> rle_data;
};

struct MaskWriteSummary {
  std::string masks_index_path;
  std::string masks_block_path;
  int mask_count = 0;
  std::vector<nlohmann::json> index_records;
  std::vector<nlohmann::json> block_manifest_entries;
};

// Write masks to spatial/masks.index.jsonl and spatial/masks.blocks.svpmz
// per spec §14.3. Each mask is written as an SVP block with dtype=svp_rle_v1.
[[nodiscard]] MaskWriteSummary write_masks(
    const std::filesystem::path& staging_dir,
    const std::vector<MaskWriteEntry>& masks);

}  // namespace svp::vision
