#pragma once

#include <memory>

struct BuildCliOptions;

namespace svp::builder {
class BuildProgressSink;
}

int run_interlace_output_build(
    const BuildCliOptions& options,
    const std::shared_ptr<svp::builder::BuildProgressSink>& progress_sink);
