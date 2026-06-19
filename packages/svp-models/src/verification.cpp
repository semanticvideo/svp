#include "svp/models/verification.hpp"

#include "svp/models/error.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/manifest.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

namespace svp::models {
namespace {

constexpr std::string_view kManifestFileName = "model.svpmodel.json";
constexpr std::string_view kLockFileName = "model-lock.json";

std::vector<std::filesystem::path> find_installed_manifests(
    const std::filesystem::path& cache_root) {
  std::vector<std::filesystem::path> manifests;
  std::error_code error;
  if (!std::filesystem::exists(cache_root, error)) {
    return manifests;
  }

  std::filesystem::recursive_directory_iterator iterator(
      cache_root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && iterator != end) {
    if (iterator->is_regular_file(error) &&
        iterator->path().filename() == kManifestFileName) {
      manifests.push_back(iterator->path());
    }
    iterator.increment(error);
  }

  return manifests;
}

void add_required_bundle_file_checks(VerificationReport& report,
                                     const std::filesystem::path& bundle_root) {
  for (const std::string_view required :
       {kManifestFileName, kLockFileName, std::string_view("LICENSE"),
        std::string_view("NOTICE")}) {
    const std::filesystem::path path = bundle_root / required;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      report.add_error("missing required model bundle file: " + path.string());
    }
  }
}

void add_lock_manifest_identity_checks(VerificationReport& report,
                                       const ModelLock& lock,
                                       const ModelBundleManifest& manifest) {
  const auto match = std::find_if(
      lock.models.begin(), lock.models.end(), [&](const ModelLockEntry& entry) {
        return entry.model_bundle_id == manifest.model_bundle_id;
      });

  if (match == lock.models.end()) {
    report.add_error("model-lock.json does not contain model_bundle_id " +
                     manifest.model_bundle_id);
    return;
  }

  if (match->model_id != manifest.model_id) {
    report.add_error("model-lock.json model_id disagrees with model.svpmodel.json for " +
                     manifest.model_bundle_id);
  }
  if (match->model_version != manifest.model_version) {
    report.add_error(
        "model-lock.json model_version disagrees with model.svpmodel.json for " +
        manifest.model_bundle_id);
  }
  if (match->bundle_blake3.canonical() != manifest.bundle_blake3.canonical()) {
    report.add_error(
        "model-lock.json bundle_blake3 disagrees with model.svpmodel.json for " +
        manifest.model_bundle_id);
  }
}

void merge_report(VerificationReport& target, const VerificationReport& source) {
  target.issues.insert(target.issues.end(), source.issues.begin(), source.issues.end());
}

std::map<std::string, std::filesystem::path> installed_bundle_manifests_by_bundle_id(
    VerificationReport& report,
    const std::filesystem::path& cache_root) {
  std::map<std::string, std::filesystem::path> manifests_by_bundle_id;
  for (const std::filesystem::path& path : find_installed_manifests(cache_root)) {
    try {
      ModelBundleManifest manifest = load_model_bundle_manifest(path);
      manifests_by_bundle_id.emplace(manifest.model_bundle_id, path);
    } catch (const ModelError& error) {
      report.add_error("could not parse installed model manifest " + path.string() +
                       ": " + error.what());
    }
  }
  return manifests_by_bundle_id;
}

}  // namespace

bool VerificationReport::ok() const noexcept {
  return std::none_of(issues.begin(), issues.end(), [](const VerificationIssue& issue) {
    return issue.severity == VerificationSeverity::error;
  });
}

void VerificationReport::add_info(std::string message) {
  issues.push_back(VerificationIssue{VerificationSeverity::info, std::move(message)});
}

void VerificationReport::add_error(std::string message) {
  issues.push_back(VerificationIssue{VerificationSeverity::error, std::move(message)});
}

VerificationReport verify_manifest_files(const ModelBundleManifest& manifest,
                                         const std::filesystem::path& bundle_root) {
  VerificationReport report;

  for (const ModelBundleFile& file : manifest.files) {
    const std::filesystem::path path = bundle_root / file.path;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      report.add_error("missing model bundle file: " + path.string());
      continue;
    }

    try {
      const std::string actual_hex = blake3_hex_for_file(path);
      if (actual_hex != file.blake3.hex_value()) {
        report.add_error("BLAKE3 mismatch for " + path.string() + ": expected " +
                         file.blake3.canonical() + ", actual blake3:" + actual_hex);
      }
    } catch (const ModelError& error) {
      report.add_error(error.what());
    }
  }

  if (report.ok()) {
    report.add_info("verified " + std::to_string(manifest.files.size()) +
                    " model bundle file hashes for " + manifest.model_bundle_id);
  }

  return report;
}

VerificationReport verify_extracted_bundle(const std::filesystem::path& bundle_root) {
  VerificationReport report;
  add_required_bundle_file_checks(report, bundle_root);

  try {
    const ModelBundleManifest manifest =
        load_model_bundle_manifest(bundle_root / kManifestFileName);
    const ModelLock lock = load_model_lock(bundle_root / kLockFileName);
    add_lock_manifest_identity_checks(report, lock, manifest);
    merge_report(report, verify_manifest_files(manifest, bundle_root));
  } catch (const ModelError& error) {
    report.add_error(error.what());
    return report;
  }

  return report;
}

VerificationReport verify_lock_against_cache(const ModelLock& lock,
                                             const std::filesystem::path& cache_root) {
  VerificationReport report;
  const auto manifests_by_bundle_id =
      installed_bundle_manifests_by_bundle_id(report, cache_root);

  for (const ModelLockEntry& entry : lock.models) {
    const auto match = manifests_by_bundle_id.find(entry.model_bundle_id);
    if (match == manifests_by_bundle_id.end()) {
      report.add_error("missing model bundle " + entry.model_bundle_id + " for " +
                       entry.model_id + " in cache " + cache_root.string());
      continue;
    }

    try {
      const std::filesystem::path bundle_root = match->second.parent_path();
      ModelBundleManifest manifest = load_model_bundle_manifest(match->second);
      add_lock_manifest_identity_checks(report, lock, manifest);
      merge_report(report, verify_manifest_files(manifest, bundle_root));
    } catch (const ModelError& error) {
      report.add_error(error.what());
    }
  }

  return report;
}

VerificationReport verify_reference_set_against_cache(
    const ReferenceModelSet& model_set,
    const std::filesystem::path& cache_root) {
  VerificationReport report;
  std::map<std::string, std::vector<std::filesystem::path>> manifests_by_model_id;

  for (const std::filesystem::path& path : find_installed_manifests(cache_root)) {
    try {
      ModelBundleManifest manifest = load_model_bundle_manifest(path);
      manifests_by_model_id[manifest.model_id].push_back(path);
    } catch (const ModelError& error) {
      report.add_error("could not parse installed model manifest " + path.string() +
                       ": " + error.what());
    }
  }

  for (const ReferenceModel& model : model_set.models) {
    const auto match = manifests_by_model_id.find(model.model_id);
    if (match == manifests_by_model_id.end() || match->second.empty()) {
      report.add_error("missing model " + model.model_id + " in cache " +
                       cache_root.string());
      continue;
    }

    for (const std::filesystem::path& manifest_path : match->second) {
      const std::filesystem::path bundle_root = manifest_path.parent_path();
      merge_report(report, verify_extracted_bundle(bundle_root));
    }
  }

  return report;
}

}  // namespace svp::models
