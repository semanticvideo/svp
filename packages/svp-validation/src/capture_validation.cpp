#include "capture_validation.hpp"

#include "json_schema_subset.hpp"
#include "record_file_reader.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace svp::validation {
namespace {

constexpr std::string_view kCaptureEntry = "provenance/capture.json";

}  // namespace

void add_capture_findings(ValidationReport& report, const ValidationCodeRegistry& registry,
                          const std::filesystem::path& package_path,
                          const svp::package::PackageLayout& layout,
                          const nlohmann::json& capture_schema) {
  if (!layout.has_entry(std::string{kCaptureEntry})) {
    return;
  }
  const std::string where = "/" + std::string{kCaptureEntry};
  const auto capture = read_json_from_package(package_path, std::string{kCaptureEntry});
  if (!capture.has_value()) {
    add_finding(report, make_finding(registry, kCodeProvenanceInvalidCapture, where,
                                     capture.error_message));
    return;
  }
  for (const auto& issue : validate_json_schema_subset(capture.value, capture_schema)) {
    add_finding(report, make_finding(registry, kCodeProvenanceInvalidCapture, where,
                                     issue.path + ": " + issue.message));
  }
}

}  // namespace svp::validation
