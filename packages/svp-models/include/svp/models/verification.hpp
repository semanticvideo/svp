#pragma once

#include "svp/models/manifest.hpp"
#include "svp/models/model_lock.hpp"
#include "svp/models/reference_model_set.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace svp::models {

enum class VerificationSeverity {
  info,
  error,
};

struct VerificationIssue {
  VerificationSeverity severity;
  std::string message;
};

struct VerificationReport {
  std::vector<VerificationIssue> issues;

  [[nodiscard]] bool ok() const noexcept;
  void add_info(std::string message);
  void add_error(std::string message);
};

[[nodiscard]] VerificationReport verify_manifest_files(
    const ModelBundleManifest& manifest,
    const std::filesystem::path& bundle_root);

[[nodiscard]] VerificationReport verify_extracted_bundle(
    const std::filesystem::path& bundle_root);

[[nodiscard]] VerificationReport verify_lock_against_cache(
    const ModelLock& lock,
    const std::filesystem::path& cache_root);

[[nodiscard]] VerificationReport verify_reference_set_against_cache(
    const ReferenceModelSet& model_set,
    const std::filesystem::path& cache_root);

}  // namespace svp::models
