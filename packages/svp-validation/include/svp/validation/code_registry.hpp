#pragma once

#include "svp/validation/report.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace svp::validation {

inline constexpr std::string_view kCodeMissingManifest = "ERR_CORE_MISSING_MANIFEST";
inline constexpr std::string_view kCodeMissingSection = "ERR_CORE_MISSING_SECTION";
inline constexpr std::string_view kCodeUnknownRootSection = "ERR_CORE_UNKNOWN_ROOT_SECTION";
inline constexpr std::string_view kCodePathTraversal = "ERR_CORE_PATH_TRAVERSAL";
inline constexpr std::string_view kCodeMissingTextSection = "ERR_CORE_MISSING_TEXT_SECTION";
inline constexpr std::string_view kCodeMissingColorSection = "ERR_CORE_MISSING_COLOR_SECTION";

inline constexpr std::string_view kTempCodeInputMissing = "X_VALIDATOR_INPUT_MISSING";
inline constexpr std::string_view kTempCodeInputNotRegularFile = "X_VALIDATOR_INPUT_NOT_REGULAR_FILE";
inline constexpr std::string_view kTempCodeWrongExtension = "X_VALIDATOR_INPUT_EXTENSION";
inline constexpr std::string_view kTempCodeZipUnreadable = "X_VALIDATOR_ZIP_UNREADABLE";
inline constexpr std::string_view kTempCodeRegistryUnreadable = "X_VALIDATOR_REGISTRY_UNREADABLE";
inline constexpr std::string_view kTempCodeRegistryInvalid = "X_VALIDATOR_REGISTRY_INVALID";
inline constexpr std::string_view kTempCodeSchemaUnreadable = "X_VALIDATOR_SCHEMA_UNREADABLE";
inline constexpr std::string_view kTempCodeSchemaInvalid = "X_VALIDATOR_SCHEMA_INVALID";

struct ValidationCode {
  std::string code;
  FindingSeverity severity = FindingSeverity::error;
  std::string report_bucket;
  bool affects_core_status = true;
  std::string status_effect;
  std::string section;
  std::string description;
};

class ValidationCodeRegistry {
 public:
  void add(ValidationCode code);

  [[nodiscard]] const ValidationCode* find(std::string_view code) const noexcept;
  [[nodiscard]] bool contains(std::string_view code) const noexcept;

 private:
  std::unordered_map<std::string, ValidationCode> codes_;
};

[[nodiscard]] ValidationCodeRegistry load_validation_code_registry(
    const std::filesystem::path& path);

[[nodiscard]] ValidationFinding make_finding(const ValidationCodeRegistry& registry,
                                             std::string_view code,
                                             std::string path,
                                             std::string message);

[[nodiscard]] ValidationFinding make_runtime_finding(std::string_view code,
                                                     std::string path,
                                                     std::string message);

}  // namespace svp::validation
