#pragma once

#include "svp/vision/mask_writer.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svp::vision {

struct VisualEntityWindowAssemblerOptions {
  // Overlap frames are decoded identically in adjacent windows. Requiring
  // 0.5 IoU makes identity handoff conservative while tolerating mask and
  // flow-boundary variation at the start of a window.
  double minimum_overlap_iou = 0.5;

  // A region occupying virtually the whole raster is scene/background
  // evidence, not a useful independently tracked entity.
  double maximum_entity_area_ratio = 0.95;

  // Three observations reject two-frame transition residue while retaining
  // entities visible for 400 ms at the default five-Hz RGB cadence.
  std::size_t minimum_observation_count = 3;

  // Reappearance without temporal overlap needs substantially stronger
  // evidence than ordinary within-window tracking. This threshold only joins
  // detector-backed regions whose normalized appearance embeddings agree.
  double minimum_reacquisition_similarity = 0.90;

  // A single crop is not enough to establish appearance identity across a
  // cut. Requiring repeated detector observations prevents one ambiguous crop
  // from stealing a later object with similar color or composition.
  std::size_t minimum_reacquisition_embedding_observations = 2;

  // Local fragments separated by a cut can use moderately weaker appearance
  // evidence only when their mean screen areas independently agree. This
  // preserves identity across short graphic interruptions without joining a
  // dominant subject to much smaller same-category background depictions.
  // A 0.60 ratio permits ordinary camera-distance changes up to 1.67x while
  // still requiring bidirectional appearance consensus.
  double minimum_supported_fragment_similarity = 0.85;
  double minimum_supported_fragment_area_similarity = 0.60;

  // Moderate-confidence re-identification is only safe after five missing
  // observations at the default 5 Hz cadence. Immediate scene-to-scene
  // handoffs still require the stronger appearance threshold above.
  std::int64_t minimum_supported_fragment_gap_us = 1'000'000;

  // Full-scene dynamic groups may briefly disappear when global motion falls
  // below the proposal threshold. Rejoin only endpoint boxes that overlap
  // strongly and are separated by no more than the decode-window overlap.
  std::int64_t maximum_motion_group_gap_us = 1'000'000;
  double minimum_motion_group_endpoint_iou = 0.50;
};

struct AssembledVisualEntityResult {
  EntityTrackResult tracker_result;
  std::vector<MaskWriteEntry> masks;
};

class VisualEntityWindowAssembler {
 public:
  explicit VisualEntityWindowAssembler(
      VisualEntityWindowAssemblerOptions options = {});

  void append_window(
      EntityTrackResult window_result,
      const std::vector<std::int64_t>& sampled_timestamps_us,
      std::int64_t overlap_start_us,
      std::int64_t emit_after_us,
      const std::vector<std::int64_t>& discontinuity_timestamps_us = {});

  [[nodiscard]] AssembledVisualEntityResult finish();

 private:
  struct EntityState {
    std::string entity_id;
    std::vector<std::string> track_ids;
    std::vector<TrackedRegion> regions;
    std::set<std::int64_t> observation_times_us;
    std::set<std::string> candidate_sources;
  };

  VisualEntityWindowAssemblerOptions options_;
  std::map<std::string, EntityState> entities_;
  std::set<std::int64_t> sampled_timestamps_us_;
  std::vector<MaskWriteEntry> masks_;
  EntityTrackResult provenance_;
  std::size_t next_entity_index_ = 1;
  std::size_t next_track_index_ = 1;
  std::size_t next_region_index_ = 1;
};

}  // namespace svp::vision
