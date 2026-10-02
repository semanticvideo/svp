#pragma once

#include <memory>

struct BuildCliOptions;

namespace svp::builder {
class BuildProgressSink;
class DistributedExecution;
}

// `distributed` is the build's paired workers (--distributed), or null.
int run_selected_output_build(
    const BuildCliOptions& options,
    const std::shared_ptr<svp::builder::BuildProgressSink>& progress_sink,
    const std::shared_ptr<svp::builder::DistributedExecution>& distributed);
