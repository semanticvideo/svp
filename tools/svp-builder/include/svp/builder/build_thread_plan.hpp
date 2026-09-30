#pragma once

#include "svp/builder/build_pipeline.hpp"
#include "svp/models/thread_plan.hpp"

namespace svp::builder {

// Resolves the one ThreadPlan a build uses. A plan supplied in `options`
// (for example by a distributed coordinator) is used as given; otherwise the
// local plan is resolved from `host` and the OCR performance profile. The
// diagnostic SVP_OCR_* thread variables are then applied on top and recorded.
// Throws std::invalid_argument when the result is not a usable plan.
[[nodiscard]] svp::models::ThreadPlanResolution resolve_build_thread_plan(
    const BuildPipelineOptions& options,
    const svp::models::HostCpuTopology& host,
    const svp::models::EnvironmentLookup& environment,
    bool diagnostic_overrides_enabled);

}  // namespace svp::builder
