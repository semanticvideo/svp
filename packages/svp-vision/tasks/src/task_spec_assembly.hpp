#pragma once

// The parts of a dispatched vision TaskSpec every type builds the same way:
// IDs over item ranges, order keys, and the RC2 §20.3 cache key over the
// type, version, source bytes (when the task reads the source), model
// bundles, and parameters.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/vision/tasks/item_batch_policy.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks::detail {

// "task.<type>.items_<first>_<last>" with six-digit (minimum) positions, last
// inclusive. Positions index the stage's item list, so IDs are stable for a
// given plan.
[[nodiscard]] std::string item_batch_task_id(std::string_view task_type, const ItemBatch& batch);

[[nodiscard]] svp::exec::TaskOrderKey item_batch_order_key(std::string_view lane,
                                                           const ItemBatch& batch);

struct SpecAssembly {
  std::string build_session_id;
  std::vector<std::string> depends_on;
  std::string task_type;
  std::uint64_t task_type_version = 0;
  std::string task_id;
  std::vector<svp::exec::TaskModelRef> model_refs;
  // The source, when the task decodes it (input name `source`).
  std::optional<svp::exec::ArtifactRef> source;
  std::string source_input_name;
  nlohmann::json parameters;
  svp::exec::TaskResources resources;
};

// The validated TaskSpec. Throws what validate_task_spec throws.
[[nodiscard]] svp::exec::TaskSpec assemble_task_spec(SpecAssembly assembly);

}  // namespace svp::vision::tasks::detail
