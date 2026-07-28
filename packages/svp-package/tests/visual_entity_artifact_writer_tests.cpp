#include "svp/package/visual_entity_artifact_writer.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

std::size_t nonempty_line_count(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::size_t count = 0;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty())
      ++count;
  }
  return count;
}

svp::vision::TrackedRegion region(const std::string& entity_id,
                                  const std::string& suffix) {
  svp::vision::TrackedRegion value;
  value.region_id = "region_" + suffix;
  value.entity_id = entity_id;
  value.track_id = "track_" + suffix;
  value.frame_id = "frame_" + suffix;
  value.mask_ref = "mask_region_" + suffix;
  return value;
}

svp::vision::MaskWriteEntry mask(const std::string& entity_id,
                                 const std::string& suffix) {
  svp::vision::MaskWriteEntry value;
  value.mask_id = "mask_region_" + suffix;
  value.entity_id = entity_id;
  value.track_id = "track_" + suffix;
  value.region_id = "region_" + suffix;
  value.frame_id = "frame_" + suffix;
  value.width = 2;
  value.height = 2;
  value.rle_data = {0, 4};
  return value;
}

void test_stream_filters_nonpersistent_entities_without_buffering_artifacts() {
  const auto root = std::filesystem::temp_directory_path() /
                    "svp-visual-entity-artifact-writer-test";
  std::filesystem::remove_all(root);
  svp::package::VisualEntityArtifactWriter writer(root);
  writer.append({region("entity_keep", "keep"), region("entity_drop", "drop")},
                {mask("entity_keep", "keep"), mask("entity_drop", "drop")});
  const auto summary = writer.finish({"entity_keep"});

  assert(summary.region_count == 1);
  assert(summary.mask_count == 1);
  assert(nonempty_line_count(root / "spatial" / "regions.jsonl") == 1);
  assert(nonempty_line_count(root / "spatial" / "masks.index.jsonl") == 1);
  const auto block_path = root / "spatial" / "masks.blocks.svpmz";
  std::ifstream mask_index(root / "spatial" / "masks.index.jsonl");
  nlohmann::json mask_record;
  mask_index >> mask_record;
  assert(std::filesystem::file_size(block_path) ==
         mask_record.at("block_length").get<std::uint64_t>());
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  test_stream_filters_nonpersistent_entities_without_buffering_artifacts();
  return 0;
}
