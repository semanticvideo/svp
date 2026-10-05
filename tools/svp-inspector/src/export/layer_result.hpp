#pragma once

#include "export_plan.hpp"
#include "reference_rules.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace package_export {

// What was written for one planned layer, as export.json reports it.
struct LayerResult {
  const PlannedLayer* plan = nullptr;
  // Size of each written file, parallel to plan->files.
  std::vector<std::uint64_t> file_sizes;
  // Records (jsonl) or blocks (block_stream); absent for json and file.
  std::optional<std::uint64_t> record_count;
  std::vector<ReferenceRule> rules;
};

}  // namespace package_export
