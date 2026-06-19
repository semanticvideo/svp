#pragma once

#include "svp/validation/report.hpp"

#include <nlohmann/json.hpp>

namespace svp::validation {

void to_json(nlohmann::json& json, const ValidationFinding& finding);
void to_json(nlohmann::json& json, const ValidationReport& report);

}  // namespace svp::validation

