#pragma once

#include <memory>
#include <string>

struct BuildCliOptions;

namespace svp::builder {
class BuildProgressSink;
class DistributedExecution;
}

// `distributed` is the build's paired workers (--distributed), or null.
// `capture_source_path` is the source as given, before --max-resolution.
int run_selected_output_build(
    const BuildCliOptions& options, const std::string& capture_source_path,
    const std::shared_ptr<svp::builder::BuildProgressSink>& progress_sink,
    const std::shared_ptr<svp::builder::DistributedExecution>& distributed);
