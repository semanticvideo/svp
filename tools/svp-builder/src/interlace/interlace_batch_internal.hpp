#pragma once

#include "svp/builder/interlace_batch.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder {

inline constexpr std::string_view kSupportedVideoExts[] = {
    ".mov", ".mp4", ".mkv", ".avi", ".webm", ".m4v", ".m4a", ".wmv", ".flv"
};

std::string make_utc_timestamp();

std::vector<std::filesystem::path> discover_media_files(
    const std::filesystem::path& dir, bool recursive);

std::vector<std::filesystem::path> discover_svpi_files(
    const std::filesystem::path& dir, bool recursive);

std::filesystem::path find_candidate_sidecar(
    const std::filesystem::path& media_path,
    const std::filesystem::path& search_dir);

std::filesystem::path media_search_dir_for_svpi(
    const std::filesystem::path& svpi_path);

std::string sidecar_stem(const std::filesystem::path& svpi_path);

bool batch_artifact_is_contained(
    const std::filesystem::path& artifact_path,
    const std::filesystem::path& output_directory,
    std::string& error_message);

// An SVPI that validates and whose media binding verifies against `media_path`.
bool check_svpi_valid_and_bound(
    const std::filesystem::path& svpi_path,
    const std::filesystem::path& media_path,
    const std::string& validation_codes_path,
    std::string& error_message);

// The blake3_state an embedded SVPI batch item reports once created.
inline constexpr std::string_view kEmbeddedBatchBlake3State = "present";

// The blake3_state an SVPI records in its media binding (empty when it
// cannot be read): what a create-batch item built on another Mac reports.
std::string svpi_blake3_state(const std::filesystem::path& svpi_path);

enum class EmbeddedBatchArtifactState {
  absent,
  valid,
  invalid,
};

EmbeddedBatchArtifactState inspect_embedded_batch_artifact(
    const std::filesystem::path& artifact_path,
    const std::filesystem::path& source_path,
    const std::filesystem::path& validation_codes_path,
    std::string& error_message);

bool check_embedded_batch_artifact(
    const std::filesystem::path& artifact_path,
    const std::filesystem::path& source_path,
    const std::filesystem::path& validation_codes_path,
    std::string& error_message);

bool create_embedded_batch_artifact(
    const BatchCreateOptions& options,
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path,
    const std::string& staging_dir,
    std::string& error_message,
    std::string& blake3_state,
    const std::shared_ptr<BuildProgressSink>& progress_sink,
    bool overwrite_output);

}  // namespace svp::builder
