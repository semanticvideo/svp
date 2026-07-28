#pragma once

#include "svp/package/package_summary.hpp"

#include <nlohmann/json.hpp>

namespace inspect_output {

[[nodiscard]] nlohmann::json package_summary_json(
    const svp::package::PackageSummary& summary);

void print_summary(const svp::package::PackageSummary& summary);

}  // namespace inspect_output
