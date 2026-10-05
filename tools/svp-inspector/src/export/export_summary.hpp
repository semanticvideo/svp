#pragma once

#include "export_plan.hpp"
#include "layer_result.hpp"
#include "package_kind.hpp"
#include "version_policy.hpp"

#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace package_export {

inline constexpr std::string_view kExportFormatName = "svp-package-export";
// Major version of the export layout and export.json (Section 10).
inline constexpr int kExportFormatVersion = 1;

struct SummaryInputs {
  PackageKind kind = PackageKind::svp;
  nlohmann::json mimetype;            // string, or null
  DeclaredVersions versions;
  nlohmann::json embedded_transport;  // object, or null
  nlohmann::json validation;          // validation_summary_json()
  nlohmann::json manifest;            // as declared
  nlohmann::json media_binding;       // as declared, or null
  nlohmann::json resolution_sources;  // ReferenceResolver::sources_json()
  const std::vector<LayerResult>* layers = nullptr;  // plan order
};

// export.json (Package_Export_v1.md Section 3). Contains nothing that
// depends on the input or output path, the time, or the host.
[[nodiscard]] nlohmann::json build_export_summary(const SummaryInputs& inputs);

}  // namespace package_export
