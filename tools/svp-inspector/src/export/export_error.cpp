#include "export_error.hpp"

#include <utility>

namespace package_export {

std::string_view to_string(ExportErrorCode code) noexcept {
  switch (code) {
    case ExportErrorCode::internal_error:
      return "internal_error";
    case ExportErrorCode::validation_unavailable:
      return "validation_unavailable";
    case ExportErrorCode::input_missing:
      return "input_missing";
    case ExportErrorCode::input_not_regular_file:
      return "input_not_regular_file";
    case ExportErrorCode::input_unrecognized:
      return "input_unrecognized";
    case ExportErrorCode::package_invalid:
      return "package_invalid";
    case ExportErrorCode::unsupported_version:
      return "unsupported_version";
    case ExportErrorCode::output_exists:
      return "output_exists";
    case ExportErrorCode::output_not_replaceable:
      return "output_not_replaceable";
    case ExportErrorCode::insufficient_space:
      return "insufficient_space";
    case ExportErrorCode::output_write_failed:
      return "output_write_failed";
    case ExportErrorCode::unsafe_entry_name:
      return "unsafe_entry_name";
    case ExportErrorCode::output_path_collision:
      return "output_path_collision";
    case ExportErrorCode::entry_unreadable:
      return "entry_unreadable";
    case ExportErrorCode::entry_size_mismatch:
      return "entry_size_mismatch";
    case ExportErrorCode::malformed_record:
      return "malformed_record";
    case ExportErrorCode::malformed_document:
      return "malformed_document";
    case ExportErrorCode::record_too_large:
      return "record_too_large";
    case ExportErrorCode::invalid_block_stream:
      return "invalid_block_stream";
    case ExportErrorCode::reserved_member_present:
      return "reserved_member_present";
    case ExportErrorCode::package_changed:
      return "package_changed";
  }
  return "internal_error";
}

int exit_code_for(ExportErrorCode code) noexcept {
  switch (code) {
    case ExportErrorCode::internal_error:
    case ExportErrorCode::validation_unavailable:
      return kExitInternal;
    case ExportErrorCode::input_missing:
    case ExportErrorCode::input_not_regular_file:
    case ExportErrorCode::input_unrecognized:
      return kExitInputUnreadable;
    case ExportErrorCode::package_invalid:
      return kExitPackageInvalid;
    case ExportErrorCode::unsupported_version:
      return kExitUnsupportedVersion;
    case ExportErrorCode::output_exists:
    case ExportErrorCode::output_not_replaceable:
    case ExportErrorCode::insufficient_space:
    case ExportErrorCode::output_write_failed:
      return kExitOutputError;
    case ExportErrorCode::unsafe_entry_name:
    case ExportErrorCode::output_path_collision:
    case ExportErrorCode::entry_unreadable:
    case ExportErrorCode::entry_size_mismatch:
    case ExportErrorCode::malformed_record:
    case ExportErrorCode::malformed_document:
    case ExportErrorCode::record_too_large:
    case ExportErrorCode::invalid_block_stream:
    case ExportErrorCode::reserved_member_present:
    case ExportErrorCode::package_changed:
      return kExitContentUnexportable;
  }
  return kExitInternal;
}

ExportError::ExportError(ExportErrorCode code, std::string message,
                         nlohmann::json details)
    : std::runtime_error(std::move(message)),
      code_(code),
      details_(std::move(details)) {}

nlohmann::json entry_details(std::string_view entry) {
  return nlohmann::json{{"entry", std::string{entry}}};
}

nlohmann::json record_details(std::string_view entry, std::uint64_t line) {
  return nlohmann::json{{"entry", std::string{entry}}, {"line", line}};
}

}  // namespace package_export
