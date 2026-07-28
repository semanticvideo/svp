#pragma once

#include "svp/vision/mask_writer.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svp::vision {

using VisualEntityArtifactSink = std::function<void(
    const std::vector<TrackedRegion>&,
    const std::vector<MaskWriteEntry>&)>;

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

  // Keep only evidence needed to hand identity into the next overlapping
  // decode window. Older heavy regions and masks are sent to artifact_sink.
  std::int64_t handoff_retention_us = 1'000'000;

  // Retain a compact appearance history for re-identification after heavy
  // region output has streamed to disk.
  std::size_t maximum_identity_evidence_regions = 16;

  VisualEntityArtifactSink artifact_sink;

  // Diagnostics and unit tests may explicitly request complete in-memory
  // artifacts. Production callers must provide artifact_sink instead.
  bool retain_artifacts_in_memory = false;
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
  struct TrackState {
    std::string track_id;
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
    std::string start_frame_id;
    std::string end_frame_id;
    std::size_t region_count = 0;
    bool reacquired = false;
    std::set<std::string> candidate_sources;
  };

  struct EntityState {
    std::string entity_id;
    std::vector<std::string> track_ids;
    std::map<std::string, TrackState> tracks;
    std::vector<TrackedRegion> regions;
    std::vector<TrackedRegion> identity_evidence;
    std::size_t observation_count = 0;
    std::int64_t first_seen_us = 0;
    std::int64_t last_seen_us = 0;
    double screen_area_sum = 0.0;
    std::set<std::string> candidate_sources;
  };

  [[nodiscard]] std::vector<TrackedRegion> reconciliation_regions(
      const EntityState& state) const;
  void remember_identity_evidence(EntityState& state,
                                  const TrackedRegion& region);
  void emit_finalized_before(std::int64_t timestamp_us);

  VisualEntityWindowAssemblerOptions options_;
  std::map<std::string, EntityState> entities_;
  std::size_t sampled_timestamp_count_ = 0;
  std::int64_t latest_sampled_timestamp_us_ = -1;
  std::map<std::string, MaskWriteEntry> pending_masks_;
  std::vector<TrackedRegion> collected_regions_;
  std::vector<MaskWriteEntry> collected_masks_;
  EntityTrackResult provenance_;
  std::size_t next_entity_index_ = 1;
  std::size_t next_track_index_ = 1;
  std::size_t next_region_index_ = 1;
};

}  // namespace svp::vision
