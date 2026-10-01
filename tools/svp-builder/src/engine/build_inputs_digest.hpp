#pragma once

// One digest over everything that decides a build's output: the source bytes,
// the ingest plan probed from them, every output-affecting option, the
// runtime tools, the thread plan, and the builder version. It is part of every
// task's parameters, so a recovery journal recorded for other inputs never
// matches the graph of this build and is refused on --resume.

#include "svp/builder/build_pipeline.hpp"
#include "svp/exec/journal_records.hpp"
#include "svp/models/thread_plan.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace svp::builder::engine {

// The digested description (canonical member order; for diagnostics).
[[nodiscard]] nlohmann::json describe_build_inputs(
    const BuildPipelineOptions& options,
    const svp::exec::SourceFingerprintRecord& source,
    const nlohmann::json& ingest_plan_json,
    const svp::models::ThreadPlan& thread_plan);

// BLAKE3 of the description's JSON text, 64 lowercase hex characters.
[[nodiscard]] std::string build_inputs_blake3(const nlohmann::json& description);

}  // namespace svp::builder::engine
