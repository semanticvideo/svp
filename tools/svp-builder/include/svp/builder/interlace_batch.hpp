#pragma once

#include "svp/builder/interlace.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace svp::builder {

enum class SidecarVisibility {
  visible,
  hidden,
  managed_dir,
};

[[nodiscard]] std::string_view sidecar_visibility_name(SidecarVisibility v) noexcept;
[[nodiscard]] std::optional<SidecarVisibility> parse_sidecar_visibility(std::string_view v) noexcept;

[[nodiscard]] std::filesystem::path resolve_sidecar_path(
    const std::filesystem::path& media_path,
    SidecarVisibility visibility,
    const std::filesystem::path& out_dir);

struct BatchCreateOptions {
  std::string source_dir;
  std::string out_dir;
  std::string model_cache_dir;
  std::string ffprobe_path = "ffprobe";
  std::string ffmpeg_path = "ffmpeg";
  std::string staging_dir;
  std::string sherpa_lib_path;
  svp::vision::InferencePerformanceOptions performance;
  bool recursive = false;
  SidecarVisibility visibility = SidecarVisibility::visible;
  bool no_blake3 = false;
  bool replace_mismatched = false;
  bool core_only_diagnostic = false;
  bool allow_fallback_diarization = false;
  bool force_single_speaker = false;
  std::shared_ptr<BuildProgressSink> progress_sink;
};

enum class BatchFileStatus {
  created,
  already_valid,
  skipped_unsupported,
  binding_mismatch,
  failed,
  replaced,
};

[[nodiscard]] std::string_view batch_file_status_label(BatchFileStatus s) noexcept;

struct BatchFileResult {
  std::string source_filename;
  std::string source_relative_path;
  std::filesystem::path svpi_path;
  BatchFileStatus status = BatchFileStatus::failed;
  std::string error_message;
  std::string blake3_state;
};

struct BatchCreateResult {
  std::vector<BatchFileResult> results;
  int created_count = 0;
  int already_valid_count = 0;
  int skipped_count = 0;
  int mismatch_count = 0;
  int failed_count = 0;
  int replaced_count = 0;
};

[[nodiscard]] BatchCreateResult interlace_create_batch(const BatchCreateOptions& options);

struct ScanPairInfo {
  std::string media_filename;
  std::string media_relative_path;
  std::filesystem::path media_path;
  std::filesystem::path svpi_path;
  bool svpi_found = false;
  bool binding_verified = false;
  std::string binding_state_label;
  std::string svpi_error;
};

struct ScanResult {
  std::vector<ScanPairInfo> pairs;
  std::vector<std::string> unbound_sidecars;
  std::vector<std::string> missing_sidecars;
  std::vector<std::string> duplicate_sidecars;
  std::vector<std::string> unsupported_media;
  int total_media = 0;
  int total_svpi = 0;
  int matched_pairs = 0;
  int verified_pairs = 0;
};

struct ScanOptions {
  std::string source_dir;
  bool recursive = false;
  std::string ffprobe_path = "ffprobe";
  std::string validation_codes_path = "spec/registries/validation-codes.json";
};

[[nodiscard]] ScanResult interlace_scan(const ScanOptions& options);

struct BatchValidateOptions {
  std::string source_dir;
  bool recursive = false;
  std::string ffprobe_path = "ffprobe";
  std::string validation_codes_path = "spec/registries/validation-codes.json";
  std::shared_ptr<BuildProgressSink> progress_sink;
};

enum class BatchValidationState {
  valid_bound,
  valid_unbound,
  binding_mismatch,
  invalid_structure,
  failed,
};

[[nodiscard]] std::string_view batch_validation_state_label(BatchValidationState s) noexcept;

struct BatchValidateFileResult {
  std::string svpi_filename;
  std::string svpi_relative_path;
  std::filesystem::path svpi_path;
  std::string media_filename;
  BatchValidationState state = BatchValidationState::failed;
  std::vector<std::string> errors;
};

struct BatchValidateResult {
  std::vector<BatchValidateFileResult> results;
  int valid_bound_count = 0;
  int valid_unbound_count = 0;
  int mismatch_count = 0;
  int invalid_structure_count = 0;
  int failed_count = 0;
};

[[nodiscard]] BatchValidateResult interlace_validate_batch(const BatchValidateOptions& options);

struct CompleteIdentityOptions {
  std::string svpi_path;
  std::string media_path;
  std::string ffprobe_path = "ffprobe";
  std::string validation_codes_path = "spec/registries/validation-codes.json";
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct CompleteIdentityResult {
  bool success = false;
  std::string error_message;
  std::string previous_state;
  std::string new_state;
  std::string blake3_hash;
  bool rebuilt = false;
};

[[nodiscard]] CompleteIdentityResult interlace_complete_identity(const CompleteIdentityOptions& options);

struct CompleteIdentityBatchOptions {
  std::string source_dir;
  bool recursive = false;
  std::string ffprobe_path = "ffprobe";
  std::string validation_codes_path = "spec/registries/validation-codes.json";
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct CompleteIdentityBatchResult {
  std::vector<CompleteIdentityResult> results;
  std::vector<std::string> svpi_filenames;
  int completed_count = 0;
  int already_present_count = 0;
  int failed_count = 0;
};

[[nodiscard]] CompleteIdentityBatchResult interlace_complete_identity_batch(
    const CompleteIdentityBatchOptions& options);

}  // namespace svp::builder
