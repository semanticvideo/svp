#pragma once

#include "svp/builder/runtime_tools.hpp"

#include <nlohmann/json.hpp>

namespace svp::builder {

// The `runtime_tools` object of the builder foundation JSON and the run
// report. The sherpa-onnx entry reports the library svp-audio actually loaded
// (if any) at the time of the call, so call it after the command's work.
[[nodiscard]] nlohmann::json runtime_tools_json(const RuntimeToolSelection& selection);

}  // namespace svp::builder
