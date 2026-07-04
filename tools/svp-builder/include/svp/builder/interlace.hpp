#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/builder/build_pipeline.hpp"
#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace svp::builder {

struct InterlaceCreateOptions {
  std::string source_path;
  std::string output_path;
  std::string staging_dir;
  std::string model_cache_dir;
  std::string ffprobe_path = "ffprobe";
  std::string ffmpeg_path = "ffmpeg";
  std::string probe_json_path;
  std::string sherpa_lib_path;
  svp::vision::InferencePerformanceOptions performance;
  bool compute_full_blake3 = true;
  bool compute_chunk_proof = true;
  bool core_only_diagnostic = false;
  bool allow_fallback_diarization = false;
  bool force_single_speaker = false;
  bool serial_pipeline = false;
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct InterlaceCreateResult {
  bool success = false;
  std::filesystem::path svpi_path;
  std::string error_message;
  std::string blake3_state;
  std::string binding_state;
};

[[nodiscard]] InterlaceCreateResult interlace_create(
    const InterlaceCreateOptions& options);

struct InterlaceValidateOptions {
  std::string svpi_path;
  std::string media_path;
  std::string ffprobe_path = "ffprobe";
  std::string validation_codes_path = "spec/registries/validation-codes.json";
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct InterlaceValidateResult {
  bool structure_valid = false;
  bool binding_verified = false;
  bool binding_attempted = false;
  std::string binding_state_label;
  std::vector<std::string> binding_passing_checks;
  std::vector<std::string> binding_failing_checks;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] InterlaceValidateResult interlace_validate(
    const InterlaceValidateOptions& options);

struct InterlaceInspectOptions {
  std::string svpi_path;
};

struct InterlaceInspectResult {
  bool success = false;
  std::string error_message;
  std::string artifact_type;
  std::string svpi_version;
  std::string manifest_format;
  std::string binding_id;
  std::string binding_contract;
  std::string binding_verification_state;
  std::string blake3_state;
  std::string blake3_hash;
  std::string media_id;
  std::string size_bytes;
  std::string duration_us;
  std::string container_format;
  std::string original_filename_hint;
  bool has_media_original = false;
  bool has_index_sqlite = false;
  bool has_index_manifest = false;
  bool has_provenance = false;
  std::vector<std::string> section_states;
  std::vector<std::string> root_entries;
  std::uint64_t entry_count = 0;
  bool recombination_ready = false;
};

[[nodiscard]] InterlaceInspectResult interlace_inspect(
    const InterlaceInspectOptions& options);

struct InterlaceExtractOptions {
  std::string svp_path;
  std::string out_dir;
  std::string validation_codes_path = "spec/registries/validation-codes.json";
  std::string ffprobe_path = "ffprobe";
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct InterlaceExtractResult {
  bool success = false;
  std::filesystem::path extracted_media_path;
  std::filesystem::path extracted_svpi_path;
  std::string error_message;
  std::string media_filename;
};

[[nodiscard]] InterlaceExtractResult interlace_extract(
    const InterlaceExtractOptions& options);

struct InterlaceRecombineOptions {
  std::string media_path;
  std::string svpi_path;
  std::string output_path;
  std::string staging_dir;
  std::string ffprobe_path = "ffprobe";
  std::string validation_codes_path = "spec/registries/validation-codes.json";
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct InterlaceRecombineResult {
  bool success = false;
  std::filesystem::path svp_path;
  bool binding_verified = false;
  std::string binding_state_label;
  std::vector<std::string> binding_passing_checks;
  std::vector<std::string> binding_failing_checks;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] InterlaceRecombineResult interlace_recombine(
    const InterlaceRecombineOptions& options);

}  // namespace svp::builder
