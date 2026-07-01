#include "svp/builder/interlace_batch.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder {

std::string_view sidecar_visibility_name(SidecarVisibility v) noexcept {
  switch (v) {
    case SidecarVisibility::visible: return "visible";
    case SidecarVisibility::hidden: return "hidden";
    case SidecarVisibility::managed_dir: return "managed-dir";
  }
  return "visible";
}

std::optional<SidecarVisibility> parse_sidecar_visibility(std::string_view v) noexcept {
  if (v == "visible") return SidecarVisibility::visible;
  if (v == "hidden") return SidecarVisibility::hidden;
  if (v == "managed-dir") return SidecarVisibility::managed_dir;
  return std::nullopt;
}

std::filesystem::path resolve_sidecar_path(
    const std::filesystem::path& media_path,
    SidecarVisibility visibility,
    const std::filesystem::path& out_dir) {
  std::string stem = media_path.stem().string();
  std::string filename;
  switch (visibility) {
    case SidecarVisibility::visible:
      filename = stem + ".svpi";
      break;
    case SidecarVisibility::hidden:
      filename = "." + stem + ".svpi";
      break;
    case SidecarVisibility::managed_dir:
      return out_dir / ".svpi" / (stem + ".svpi");
  }
  return out_dir / filename;
}

std::string_view batch_file_status_label(BatchFileStatus s) noexcept {
  switch (s) {
    case BatchFileStatus::created: return "created";
    case BatchFileStatus::already_valid: return "already_valid";
    case BatchFileStatus::skipped_unsupported: return "skipped_unsupported";
    case BatchFileStatus::binding_mismatch: return "binding_mismatch";
    case BatchFileStatus::failed: return "failed";
    case BatchFileStatus::replaced: return "replaced";
  }
  return "failed";
}

std::string_view batch_validation_state_label(BatchValidationState s) noexcept {
  switch (s) {
    case BatchValidationState::valid_bound: return "valid_bound";
    case BatchValidationState::valid_unbound: return "valid_unbound";
    case BatchValidationState::binding_mismatch: return "binding_mismatch";
    case BatchValidationState::invalid_structure: return "invalid_structure";
    case BatchValidationState::failed: return "failed";
  }
  return "failed";
}

}  // namespace svp::builder
