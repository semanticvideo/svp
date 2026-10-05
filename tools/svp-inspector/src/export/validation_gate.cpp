#include "validation_gate.hpp"

#include "svp/validation/code_registry.hpp"
#include "svp/validation/embedded_svpi_transport_validator.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/validator.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace package_export {
namespace {

using svp::validation::ValidationFinding;
using svp::validation::ValidationReport;

bool has_code(const std::vector<ValidationFinding>& findings,
              std::string_view code) {
  return std::ranges::any_of(findings, [&](const ValidationFinding& finding) {
    return finding.code == code;
  });
}

nlohmann::json code_counts(const std::vector<ValidationFinding>& findings) {
  std::map<std::string, std::uint64_t> counts;
  for (const auto& finding : findings) {
    ++counts[finding.code];
  }
  auto result = nlohmann::json::array();
  for (const auto& [code, count] : counts) {
    result.push_back({{"code", code}, {"count", count}});
  }
  return result;
}

}  // namespace

ValidationReport validate_for_export(const std::filesystem::path& path,
                                     PackageKind kind) {
  switch (kind) {
    case PackageKind::embedded_svpi:
      return svp::validation::validate_embedded_svpi_transport(path, {});
    case PackageKind::svpi:
      return svp::validation::validate_svpi_package(path, {});
    case PackageKind::svp:
      return svp::validation::validate_package(path, {});
  }
  return svp::validation::validate_package(path, {});
}

bool validation_passed(const ValidationReport& report) noexcept {
  return svp::validation::exit_code(report) == 0;
}

bool validation_resources_unavailable(const ValidationReport& report) noexcept {
  constexpr std::array<std::string_view, 4> kResourceCodes{
      svp::validation::kTempCodeRegistryUnreadable,
      svp::validation::kTempCodeRegistryInvalid,
      svp::validation::kTempCodeSchemaUnreadable,
      svp::validation::kTempCodeSchemaInvalid,
  };
  return report.status == svp::validation::ValidationStatus::unreadable &&
         std::ranges::any_of(kResourceCodes, [&](std::string_view code) {
           return has_code(report.errors, code);
         });
}

bool embedded_profile_unsupported(const ValidationReport& report) noexcept {
  return report.embedding_transport.present &&
         has_code(report.errors,
                  svp::validation::kCodeIsoBmffSvpiProfileUnsupported);
}

nlohmann::json validation_summary_json(const ValidationReport& report) {
  return nlohmann::json{
      {"validator",
       {{"name", report.validator.name},
        {"version", report.validator.version}}},
      {"status", svp::validation::to_string(report.status)},
      {"core_status", svp::validation::to_string(report.core_status)},
      {"errors", code_counts(report.errors)},
      {"warnings", code_counts(report.warnings)},
      {"infos", code_counts(report.infos)},
  };
}

}  // namespace package_export
