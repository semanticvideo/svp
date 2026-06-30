#pragma once

#include "svp/package/media_binding.hpp"

#include <filesystem>
#include <string>

namespace svp::package {

struct MediaBindingFactoryOptions {
  std::string ffprobe_path = "ffprobe";
  std::string binding_id = "mb_primary_000001";
  std::string media_id = "media_src_000001";
  bool compute_full_blake3 = true;
  bool compute_chunk_proof = true;
  std::int64_t chunk_size_bytes = 16777216;
};

[[nodiscard]] MediaBindingDocument create_media_binding(
    const std::filesystem::path& source_path,
    const MediaBindingFactoryOptions& options = {});

enum class BindingVerificationState {
  verified,
  mismatch,
  pending,
  unavailable,
};

struct BindingVerificationResult {
  BindingVerificationState state = BindingVerificationState::unavailable;
  std::string state_label;
  std::vector<std::string> passing_checks;
  std::vector<std::string> failing_checks;
  std::string candidate_path;
  std::int64_t candidate_size_bytes = 0;
  std::string candidate_blake3;
};

[[nodiscard]] BindingVerificationResult verify_media_binding(
    const std::filesystem::path& candidate_path,
    const MediaBindingDocument& binding_doc);

[[nodiscard]] MediaBindingDocument parse_media_binding_json(
    const std::string& json_content);

}  // namespace svp::package
