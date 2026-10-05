#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace package_export {

// Every way `svp-inspector export` can refuse or fail. The string form and the
// process exit code of each value are part of the export contract
// (docs/svpi/Package_Export_v1.md, Section 9).
enum class ExportErrorCode {
  internal_error,
  validation_unavailable,
  input_missing,
  input_not_regular_file,
  input_unrecognized,
  package_invalid,
  unsupported_version,
  output_exists,
  output_not_replaceable,
  insufficient_space,
  output_write_failed,
  unsafe_entry_name,
  output_path_collision,
  entry_unreadable,
  entry_size_mismatch,
  malformed_record,
  malformed_document,
  record_too_large,
  invalid_block_stream,
  reserved_member_present,
  package_changed,
};

// Process exit codes, grouped by who has to act on the failure.
inline constexpr int kExitExported = 0;
inline constexpr int kExitInternal = 1;
inline constexpr int kExitInputUnreadable = 2;
inline constexpr int kExitPackageInvalid = 3;
inline constexpr int kExitUnsupportedVersion = 4;
inline constexpr int kExitOutputError = 5;
inline constexpr int kExitContentUnexportable = 6;

[[nodiscard]] std::string_view to_string(ExportErrorCode code) noexcept;
[[nodiscard]] int exit_code_for(ExportErrorCode code) noexcept;

class ExportError : public std::runtime_error {
 public:
  ExportError(ExportErrorCode code, std::string message,
              nlohmann::json details = nlohmann::json::object());

  [[nodiscard]] ExportErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const nlohmann::json& details() const noexcept {
    return details_;
  }

 private:
  ExportErrorCode code_;
  nlohmann::json details_;
};

// Details object naming the package entry (and record line, when known)
// that could not be exported.
[[nodiscard]] nlohmann::json entry_details(std::string_view entry);
[[nodiscard]] nlohmann::json record_details(std::string_view entry,
                                            std::uint64_t line);

}  // namespace package_export
