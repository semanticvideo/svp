#pragma once

#include "svp/vision/visual_entity_window_assembler.hpp"

#include <cstdint>
#include <vector>

namespace svp::vision::identity_reconciliation {

[[nodiscard]] double overlap_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    std::int64_t overlap_start_us);

[[nodiscard]] double motion_group_reacquisition_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    const VisualEntityWindowAssemblerOptions& options);

[[nodiscard]] double supported_reacquisition_score(
    const std::vector<TrackedRegion>& previous,
    const std::vector<TrackedRegion>& current,
    const VisualEntityWindowAssemblerOptions& options);

}  // namespace svp::vision::identity_reconciliation
