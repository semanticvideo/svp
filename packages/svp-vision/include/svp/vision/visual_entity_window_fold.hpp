#pragma once

// The visual tracking stage's reduction over window outcomes
// (visual_entity_window.hpp), in window order: frame registration, decode
// counters, the deduplicated failure list and cut evidence, each window's
// provenance additions, VisualEntityWindowAssembler::append_window, and
// finally the stage's status and detector diagnostics. Every counter-based ID
// (entity, track, region, mask) is assigned here, so where or in what order
// windows were computed is never observable.

#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_window.hpp"
#include "svp/vision/visual_entity_window_assembler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace svp::vision {

class VisualEntityWindowFold {
 public:
  // Records the runtimes' load blockers first, as the stage always has.
  // `frame_catalog`, for windows decoded without it (elsewhere), receives
  // each window's decoded frames as its outcome is appended; null when the
  // windows registered their frames while decoding. `options` and `plan`
  // must outlive the fold; `plan` must have at least one window.
  VisualEntityWindowFold(const VisualEntityPipelineOptions& options,
                         const VisualEntityPipelinePlan& plan,
                         VisualEntityWindowRuntimeStatus runtimes,
                         FrameCatalog* frame_catalog);

  // Appends the outcome of the next window. Throws std::logic_error when
  // `window_index` is not the next window.
  void append(std::size_t window_index, VisualEntityWindowOutcome outcome);

  // Valid once every window has been appended.
  [[nodiscard]] VisualEntityPipelineResult finish();

 private:
  void record_failure(const std::string& component, const std::string& message,
                      std::int64_t window_start_us, std::int64_t window_end_us);
  void add_window_provenance(EntityTrackResult& window_result) const;

  const VisualEntityPipelineOptions& options_;
  const VisualEntityPipelinePlan& plan_;
  VisualEntityWindowRuntimeStatus runtimes_;
  FrameCatalog* frame_catalog_;
  VisualEntityWindowAssembler assembler_;
  VisualEntityPipelineResult result_;
  VisualEntityDetectorDiagnostics detector_diagnostics_;
  std::int64_t previous_window_end_us_ = -1;
  std::size_t next_window_ = 0;
};

}  // namespace svp::vision
