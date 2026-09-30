#include "svp/builder/build_thread_plan.hpp"

#include "svp/vision/inference_performance.hpp"

namespace svp::builder {

svp::models::ThreadPlanResolution resolve_build_thread_plan(
    const BuildPipelineOptions& options,
    const svp::models::HostCpuTopology& host,
    const svp::models::EnvironmentLookup& environment,
    bool diagnostic_overrides_enabled) {
  svp::models::ThreadPlanResolution resolution;
  resolution.host = host;
  if (options.thread_plan) {
    resolution.plan = *options.thread_plan;
    resolution.source = svp::models::ThreadPlanSource::supplied;
  } else {
    resolution.plan = svp::models::resolve_local_thread_plan(
        host, svp::vision::recognition_workers_for_ocr_profile(
                  options.performance.ocr_performance_profile));
    resolution.source = svp::models::ThreadPlanSource::host;
  }
  resolution.overrides = svp::models::apply_thread_plan_environment_overrides(
      resolution.plan, environment, diagnostic_overrides_enabled);
  svp::models::require_valid_thread_plan(resolution.plan);
  return resolution;
}

}  // namespace svp::builder
