#include "export_plan.hpp"

#include "entry_path_policy.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace package_export {
namespace {

// SVP RC2 Section 6 / 5.12 and SVPI v0.1 Section 8.1 name these files.
constexpr std::string_view kJsonLinesSuffix = ".jsonl";
constexpr std::string_view kJsonSuffix = ".json";
constexpr std::array<std::string_view, 3> kBlockStreamSuffixes{
    ".svpdz",  // spatial/depth.blocks.svpdz
    ".svpmz",  // spatial/masks.blocks.svpmz
    ".svpez",  // embeddings/embeddings.blocks.svpez
};

std::optional<std::string> section_for(std::string_view entry_name) {
  const auto slash = entry_name.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string{entry_name.substr(0, slash)};
}

std::vector<PlannedFile> files_for(std::string_view entry_name,
                                   Representation representation) {
  const auto mirrored = layer_file_path(entry_name);
  switch (representation) {
    case Representation::jsonl:
      return {{mirrored, OutputRole::records}};
    case Representation::json:
      return {{mirrored, OutputRole::document}};
    case Representation::block_stream:
      return {
          {mirrored + std::string{kBlockTableSuffix}, OutputRole::block_table},
          {mirrored + std::string{kDecodedPayloadSuffix},
           OutputRole::decoded_payloads},
      };
    case Representation::file:
      return {{mirrored, OutputRole::bytes}};
  }
  return {{mirrored, OutputRole::bytes}};
}

std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  return rhs > std::numeric_limits<std::uint64_t>::max() - lhs
             ? std::numeric_limits<std::uint64_t>::max()
             : lhs + rhs;
}

}  // namespace

std::string_view to_string(Representation representation) noexcept {
  switch (representation) {
    case Representation::jsonl:
      return "jsonl";
    case Representation::json:
      return "json";
    case Representation::block_stream:
      return "block_stream";
    case Representation::file:
      return "file";
  }
  return "file";
}

std::string_view to_string(OutputRole role) noexcept {
  switch (role) {
    case OutputRole::records:
      return "records";
    case OutputRole::document:
      return "document";
    case OutputRole::bytes:
      return "bytes";
    case OutputRole::block_table:
      return "block_table";
    case OutputRole::decoded_payloads:
      return "decoded_payloads";
  }
  return "bytes";
}

Representation representation_for(std::string_view entry_name) {
  if (entry_name.ends_with(kJsonLinesSuffix)) {
    return Representation::jsonl;
  }
  if (entry_name.ends_with(kJsonSuffix)) {
    return Representation::json;
  }
  if (std::ranges::any_of(kBlockStreamSuffixes, [&](std::string_view suffix) {
        return entry_name.ends_with(suffix);
      })) {
    return Representation::block_stream;
  }
  return Representation::file;
}

std::string layer_file_path(std::string_view entry_name) {
  std::string path{kLayersDirectoryName};
  path.push_back('/');
  path.append(entry_name);
  return path;
}

const PlannedLayer* ExportPlan::find(std::string_view entry_name) const {
  const auto found = std::ranges::lower_bound(
      layers, entry_name, {},
      [](const PlannedLayer& layer) -> std::string_view {
        return layer.entry.name;
      });
  if (found == layers.end() || found->entry.name != entry_name) {
    return nullptr;
  }
  return &*found;
}

ExportPlan make_export_plan(const std::vector<PackageEntry>& entries) {
  ExportPlan plan;
  std::vector<PlannedOutputPath> outputs;
  for (const auto& entry : entries) {
    require_safe_entry_name(entry.name, entry.is_directory);
    if (entry.is_directory) {
      continue;
    }
    PlannedLayer layer;
    layer.entry = entry;
    layer.representation = representation_for(entry.name);
    layer.section = section_for(entry.name);
    layer.files = files_for(entry.name, layer.representation);
    for (const auto& file : layer.files) {
      require_output_segments_fit(file.path, entry.name);
      outputs.push_back({file.path, entry.name});
    }
    plan.declared_input_bytes =
        saturating_add(plan.declared_input_bytes, entry.size_bytes);
    plan.layers.push_back(std::move(layer));
  }
  require_no_output_collisions(outputs);
  std::ranges::sort(plan.layers, {}, [](const PlannedLayer& layer) {
    return std::string_view{layer.entry.name};
  });
  return plan;
}

}  // namespace package_export
