#include "task_spec_assembly.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cache_key.hpp"
#include "svp/exec/parameters_digest.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace svp::vision::tasks::detail {
namespace {

std::string padded_position(std::uint64_t position) {
  std::ostringstream oss;
  oss << std::setw(6) << std::setfill('0') << position;
  return oss.str();
}

}  // namespace

std::string item_batch_task_id(std::string_view task_type, const ItemBatch& batch) {
  const std::uint64_t last = batch.first + batch.count - 1;
  return "task." + std::string(task_type) + ".items_" + padded_position(batch.first) + "_" +
         padded_position(last);
}

svp::exec::TaskOrderKey item_batch_order_key(std::string_view lane, const ItemBatch& batch) {
  return svp::exec::TaskOrderKey{.lane = std::string(lane), .ordinals = {batch.first}};
}

svp::exec::TaskSpec assemble_task_spec(SpecAssembly assembly) {
  svp::exec::TaskSpec spec;
  spec.build_session_id = std::move(assembly.build_session_id);
  spec.task_id = std::move(assembly.task_id);
  spec.task_type = std::move(assembly.task_type);
  spec.task_type_version = assembly.task_type_version;
  spec.depends_on = std::move(assembly.depends_on);
  std::sort(spec.depends_on.begin(), spec.depends_on.end());
  spec.model_refs = std::move(assembly.model_refs);
  std::sort(spec.model_refs.begin(), spec.model_refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  if (assembly.source) {
    spec.inputs.emplace(assembly.source_input_name, *assembly.source);
  }
  spec.parameters = std::move(assembly.parameters);
  spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);

  nlohmann::json bundle_ids = nlohmann::json::array();
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    bundle_ids.push_back(ref.model_bundle_id);
  }
  // A task without a source input keys on its parameters alone (they carry
  // every item it embeds).
  spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array({
      spec.task_type,
      spec.task_type_version,
      assembly.source ? svp::exec::blake3_hex(assembly.source->blake3) : std::string(),
      std::move(bundle_ids),
      svp::exec::blake3_hex(spec.parameters_blake3),
  }));
  spec.resources = assembly.resources;
  svp::exec::validate_task_spec(spec);
  return spec;
}

}  // namespace svp::vision::tasks::detail
