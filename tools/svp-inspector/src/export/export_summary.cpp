#include "export_summary.hpp"

#include "svp/core/version.hpp"

#include <map>
#include <set>
#include <string>

namespace package_export {
namespace {

constexpr std::string_view kExporterTool = "svp-inspector";

nlohmann::json member_or_null(const nlohmann::json& object,
                              const std::string& name) {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto found = object.find(name);
  return found == object.end() ? nlohmann::json(nullptr) : *found;
}

void add_object_keys(const nlohmann::json& object,
                     std::set<std::string>& names) {
  if (!object.is_object()) {
    return;
  }
  for (const auto& [name, value] : object.items()) {
    names.insert(name);
  }
}

std::string_view layer_state(const LayerResult& layer) {
  switch (layer.plan->representation) {
    case Representation::jsonl:
    case Representation::block_stream:
      return layer.record_count.value_or(0) > 0 ? "present" : "empty";
    case Representation::json:
      return "present";
    case Representation::file:
      return layer.plan->entry.size_bytes > 0 ? "present" : "empty";
  }
  return "present";
}

nlohmann::json layer_json(const LayerResult& layer) {
  const auto& plan = *layer.plan;
  auto files = nlohmann::json::array();
  for (std::size_t index = 0; index < plan.files.size(); ++index) {
    files.push_back({
        {"path", plan.files[index].path},
        {"role", to_string(plan.files[index].role)},
        {"size_bytes", layer.file_sizes.at(index)},
    });
  }
  auto resolutions = nlohmann::json::array();
  for (const auto& rule : layer.rules) {
    resolutions.push_back({{"member", std::string{rule.member}},
                           {"kind", to_string(rule.kind)}});
  }
  return {
      {"entry", plan.entry.name},
      {"section", plan.section ? nlohmann::json(*plan.section)
                               : nlohmann::json(nullptr)},
      {"representation", to_string(plan.representation)},
      {"files", std::move(files)},
      {"record_count", layer.record_count ? nlohmann::json(*layer.record_count)
                                          : nlohmann::json(nullptr)},
      {"state", layer_state(layer)},
      {"source_size_bytes", plan.entry.size_bytes},
      {"resolutions", std::move(resolutions)},
  };
}

nlohmann::json sections_json(const nlohmann::json& manifest,
                             const std::vector<LayerResult>& layers) {
  const auto declared = member_or_null(manifest, "sections");
  const auto required = member_or_null(manifest, "required_sections");
  std::set<std::string> names;
  add_object_keys(declared, names);
  add_object_keys(required, names);
  std::map<std::string, std::uint64_t> layer_counts;
  for (const auto& layer : layers) {
    if (layer.plan->section) {
      names.insert(*layer.plan->section);
      ++layer_counts[*layer.plan->section];
    }
  }

  auto sections = nlohmann::json::array();
  for (const auto& name : names) {
    const auto declaration = member_or_null(declared, name);
    const auto state = member_or_null(declaration, "state");
    const auto count = layer_counts.find(name);
    sections.push_back({
        {"name", name},
        {"declared_state", state.is_string() ? state : nlohmann::json(nullptr)},
        {"declaration", declaration},
        {"declared_required", member_or_null(required, name)},
        {"layer_count", count == layer_counts.end() ? 0 : count->second},
    });
  }
  return sections;
}

}  // namespace

nlohmann::json build_export_summary(const SummaryInputs& inputs) {
  const auto& layers = *inputs.layers;
  auto layer_list = nlohmann::json::array();
  for (const auto& layer : layers) {
    layer_list.push_back(layer_json(layer));
  }
  return {
      {"export_format", std::string{kExportFormatName}},
      {"export_format_version", kExportFormatVersion},
      {"exporter",
       {{"tool", std::string{kExporterTool}},
        {"tool_version", std::string{svp::core::kToolVersion}}}},
      {"package",
       {
           {"kind", to_string(inputs.kind)},
           {"mimetype", inputs.mimetype},
           {"format_versions", to_json(inputs.versions)},
           {"embedded_transport", inputs.embedded_transport},
       }},
      {"validation", inputs.validation},
      {"manifest", inputs.manifest},
      {"media_binding", inputs.media_binding},
      {"sections", sections_json(inputs.manifest, layers)},
      {"resolution_sources", inputs.resolution_sources},
      {"layers", std::move(layer_list)},
  };
}

}  // namespace package_export
