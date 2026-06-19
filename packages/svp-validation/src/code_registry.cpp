#include "svp/validation/code_registry.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <utility>

namespace svp::validation {
namespace {

FindingSeverity parse_severity(const std::string& severity) {
  if (severity == "info") {
    return FindingSeverity::info;
  }
  if (severity == "warning") {
    return FindingSeverity::warning;
  }
  if (severity == "error") {
    return FindingSeverity::error;
  }
  if (severity == "fatal") {
    return FindingSeverity::fatal;
  }

  throw std::runtime_error("unknown validation-code severity: " + severity);
}

ValidationCode make_temp_code(std::string_view code,
                              FindingSeverity severity,
                              std::string description) {
  return ValidationCode{
      .code = std::string{code},
      .severity = severity,
      .report_bucket = severity == FindingSeverity::warning ? "warnings" : "errors",
      .affects_core_status = true,
      .status_effect = severity == FindingSeverity::warning ? "valid_with_warnings"
                                                            : "unreadable",
      .section = "validator-runtime",
      .description = std::move(description),
  };
}

void add_temporary_validator_codes(ValidationCodeRegistry& registry) {
  registry.add(make_temp_code(kTempCodeInputMissing, FindingSeverity::fatal,
                              "The requested input file does not exist."));
  registry.add(make_temp_code(kTempCodeInputNotRegularFile, FindingSeverity::fatal,
                              "The requested input path is not a regular file."));
  registry.add(make_temp_code(kTempCodeWrongExtension, FindingSeverity::fatal,
                              "The requested input path does not use the .svp extension."));
  registry.add(make_temp_code(kTempCodeZipUnreadable, FindingSeverity::fatal,
                              "The requested .svp file could not be opened as a ZIP package."));
  registry.add(make_temp_code(kTempCodeRegistryUnreadable, FindingSeverity::fatal,
                              "The validation-code registry could not be read."));
  registry.add(make_temp_code(kTempCodeRegistryInvalid, FindingSeverity::fatal,
                              "The validation-code registry is malformed."));
  registry.add(make_temp_code(kTempCodeSchemaUnreadable, FindingSeverity::fatal,
                              "A required schema asset could not be read."));
  registry.add(make_temp_code(kTempCodeSchemaInvalid, FindingSeverity::fatal,
                              "A required schema asset is malformed."));
  registry.add(make_temp_code(
      kTempCodeColorInvalidBucketRegistryVersion, FindingSeverity::error,
      "A color observation uses an unsupported color bucket registry version."));
}

}  // namespace

void ValidationCodeRegistry::add(ValidationCode code) {
  codes_.insert_or_assign(code.code, std::move(code));
}

const ValidationCode* ValidationCodeRegistry::find(std::string_view code) const noexcept {
  const auto found = codes_.find(std::string{code});
  if (found == codes_.end()) {
    return nullptr;
  }

  return &found->second;
}

bool ValidationCodeRegistry::contains(std::string_view code) const noexcept {
  return find(code) != nullptr;
}

ValidationCodeRegistry load_validation_code_registry(const std::filesystem::path& path) {
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error("could not open validation-code registry: " + path.string());
  }

  nlohmann::json registry_json;
  input >> registry_json;

  ValidationCodeRegistry registry;
  const auto& codes = registry_json.at("codes");
  for (const auto& code_json : codes) {
    ValidationCode code;
    code.code = code_json.at("code").get<std::string>();
    code.severity = parse_severity(code_json.at("severity").get<std::string>());
    code.report_bucket = code_json.at("report_bucket").get<std::string>();
    code.affects_core_status = code_json.at("affects_core_status").get<bool>();
    code.status_effect = code_json.at("status_effect").get<std::string>();
    code.section = code_json.at("section").get<std::string>();
    code.description = code_json.at("description").get<std::string>();
    registry.add(std::move(code));
  }

  add_temporary_validator_codes(registry);
  return registry;
}

ValidationFinding make_finding(const ValidationCodeRegistry& registry,
                               std::string_view code,
                               std::string path,
                               std::string message) {
  const auto* registered_code = registry.find(code);
  if (registered_code == nullptr) {
    throw std::runtime_error("validation code is not registered: " + std::string{code});
  }

  return ValidationFinding{
      .code = registered_code->code,
      .severity = registered_code->severity,
      .message = std::move(message),
      .path = std::move(path),
      .affects_core_status = registered_code->affects_core_status,
  };
}

ValidationFinding make_runtime_finding(std::string_view code,
                                       std::string path,
                                       std::string message) {
  ValidationCodeRegistry registry;
  add_temporary_validator_codes(registry);
  return make_finding(registry, code, std::move(path), std::move(message));
}

}  // namespace svp::validation
