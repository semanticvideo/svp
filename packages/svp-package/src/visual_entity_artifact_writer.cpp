#include "svp/package/visual_entity_artifact_writer.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace svp::package {
namespace {

nlohmann::json region_record(const svp::vision::TrackedRegion& region) {
  return {{"id", region.region_id},
          {"entity_id", region.entity_id},
          {"track_id", region.track_id},
          {"frame_id", region.frame_id},
          {"pts_us", region.timestamp_us},
          {"box_norm",
           {region.box_norm[0], region.box_norm[1], region.box_norm[2],
            region.box_norm[3]}},
          {"box_px",
           {region.box_px[0], region.box_px[1], region.box_px[2],
            region.box_px[3]}},
          {"centroid_norm", {region.centroid_norm[0], region.centroid_norm[1]}},
          {"screen_area_ratio", region.screen_area_ratio},
          {"mask_ref", region.mask_ref},
          {"depth_ref", region.depth_ref},
          {"depth_summary",
           {{"median_inverse_depth", region.median_inverse_depth},
            {"near_percentile_10", region.near_percentile_10},
            {"far_percentile_90", region.far_percentile_90}}},
          {"confidence", region.confidence},
          {"candidate_source", region.candidate_source}};
}

}  // namespace

struct VisualEntityArtifactWriter::Impl {
  explicit Impl(const std::filesystem::path& staging_dir)
      : regions_path(staging_dir / "spatial" / "regions.jsonl"),
        regions_tmp_path(staging_dir / "spatial" / "regions.jsonl.tmp"),
        masks(staging_dir) {
    std::filesystem::create_directories(regions_path.parent_path());
    regions_tmp.open(regions_tmp_path);
    if (!regions_tmp) {
      throw std::runtime_error("failed to open streamed visual regions");
    }
  }

  std::filesystem::path regions_path;
  std::filesystem::path regions_tmp_path;
  std::ofstream regions_tmp;
  svp::vision::MaskStreamWriter masks;
  bool finished = false;
};

VisualEntityArtifactWriter::VisualEntityArtifactWriter(
    const std::filesystem::path& staging_dir)
    : impl_(std::make_unique<Impl>(staging_dir)) {}

VisualEntityArtifactWriter::~VisualEntityArtifactWriter() {
  if (!impl_ || impl_->finished)
    return;
  impl_->regions_tmp.close();
  std::error_code error;
  std::filesystem::remove(impl_->regions_tmp_path, error);
}

void VisualEntityArtifactWriter::append(
    const std::vector<svp::vision::TrackedRegion>& regions,
    const std::vector<svp::vision::MaskWriteEntry>& masks) {
  if (!impl_ || impl_->finished) {
    throw std::logic_error("cannot append to a finished artifact stream");
  }
  for (const auto& region : regions) {
    impl_->regions_tmp << region_record(region).dump() << '\n';
  }
  for (const auto& mask : masks)
    impl_->masks.append(mask);
  if (!impl_->regions_tmp) {
    throw std::runtime_error("failed to append streamed visual regions");
  }
}

VisualEntityArtifactStreamSummary VisualEntityArtifactWriter::finish(
    const std::set<std::string>& retained_entity_ids) {
  if (!impl_ || impl_->finished) {
    throw std::logic_error("artifact stream was already finished");
  }
  impl_->regions_tmp.close();
  std::ifstream input(impl_->regions_tmp_path);
  std::ofstream output(impl_->regions_path);
  if (!input || !output) {
    throw std::runtime_error("failed to finalize streamed visual regions");
  }
  VisualEntityArtifactStreamSummary summary;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto record = nlohmann::json::parse(line);
    if (retained_entity_ids.count(record.at("entity_id").get<std::string>()) ==
        0) {
      continue;
    }
    output << record.dump() << '\n';
    ++summary.region_count;
  }
  input.close();
  output.close();
  std::filesystem::remove(impl_->regions_tmp_path);
  summary.mask_count = impl_->masks.finish(&retained_entity_ids).mask_count;
  impl_->finished = true;
  return summary;
}

}  // namespace svp::package
