#include "svp/models/error.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/model_lock.hpp"
#include "svp/models/verification.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kDigest =
    "blake3:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kZeroDigest =
    "blake3:0000000000000000000000000000000000000000000000000000000000000000";

namespace fs = std::filesystem;

struct TemporaryDirectory {
  fs::path path;

  TemporaryDirectory() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("svp-model-lock-tests-" + std::to_string(unique));
    fs::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

bool has_error_containing(const svp::models::VerificationReport& report,
                          std::string_view text) {
  for (const auto& issue : report.issues) {
    if (issue.severity == svp::models::VerificationSeverity::error &&
        issue.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void write_text(const fs::path& path, std::string_view value) {
  std::ofstream output(path, std::ios::binary);
  output << value;
}

std::string file_digest(const fs::path& path) {
  return "blake3:" + svp::models::blake3_hex_for_file(path);
}

nlohmann::json write_valid_bundle(const fs::path& bundle_root,
                                  std::string_view model_id,
                                  std::string_view model_version) {
  fs::create_directories(bundle_root);
  write_text(bundle_root / "model.onnx", "model bytes");
  write_text(bundle_root / "LICENSE", "test license\n");
  write_text(bundle_root / "NOTICE", "test notice\n");

  nlohmann::json manifest = {
      {"schema_version", "svp-model-bundle-1"},
      {"model_bundle_id",
       std::string(model_id) + "@" + std::string(model_version) +
           "+blake3_000000000000"},
      {"model_id", model_id},
      {"model_version", model_version},
      {"bundle_blake3", kZeroDigest},
      {"runtime", "onnxruntime"},
      {"format", "onnx"},
      {"license", "Apache-2.0"},
      {"supported_execution_providers", nlohmann::json::array({"cpu"})},
      {"files",
       nlohmann::json::array(
           {{{"path", "model.onnx"},
             {"role", "model"},
             {"blake3", file_digest(bundle_root / "model.onnx")}},
            {{"path", "LICENSE"},
             {"role", "license"},
             {"blake3", file_digest(bundle_root / "LICENSE")}},
            {{"path", "NOTICE"},
             {"role", "notice"},
             {"blake3", file_digest(bundle_root / "NOTICE")}}})},
      {"input_contract", nlohmann::json::object()},
      {"output_contract", nlohmann::json::object()},
      {"preprocessor_contract", nlohmann::json::object()},
      {"postprocessor_contract", nlohmann::json::object()},
  };
  write_text(bundle_root / "model.svpmodel.json", manifest.dump(2) + "\n");
  const std::string digest =
      "blake3:" + svp::models::blake3_hex_for_model_bundle(bundle_root);
  manifest["bundle_blake3"] = digest;
  manifest["model_bundle_id"] =
      std::string(model_id) + "@" + std::string(model_version) +
      "+blake3_" + digest.substr(7, 12);
  write_text(bundle_root / "model.svpmodel.json", manifest.dump(2) + "\n");
  return manifest;
}

svp::models::ModelLock lock_for_manifest(const nlohmann::json& manifest) {
  nlohmann::json value = {
      {"schema_version", "svp-model-lock-1"},
      {"model_set_id", "verification-test"},
      {"models",
       nlohmann::json::array(
           {{{"model_id", manifest["model_id"]},
             {"model_bundle_id", manifest["model_bundle_id"]},
             {"model_version", manifest["model_version"]},
             {"bundle_blake3", manifest["bundle_blake3"]},
             {"files", manifest["files"]}}})},
  };
  return svp::models::parse_model_lock(value, "verification-lock.json");
}

template <typename Function>
void expect_model_error(Function&& function, std::string_view message) {
  bool rejected = false;
  try {
    function();
  } catch (const svp::models::ModelError&) {
    rejected = true;
  }
  expect(rejected, message);
}

nlohmann::json lock_json() {
  return {
      {"schema_version", "svp-model-lock-1"},
      {"model_set_id", "reference-set"},
      {"models",
       nlohmann::json::array(
           {{{"model_id", "model_lock_test"},
             {"model_bundle_id", "model_lock_test@1.0+blake3_111111111111"},
             {"model_version", "1.0"},
             {"bundle_blake3", kDigest},
             {"files",
              nlohmann::json::array({{{"path", "model.onnx"},
                                      {"role", "model"},
                                      {"blake3", kDigest}}})}}})}};
}

void test_complete_file_inventory_is_required() {
  const auto parsed = svp::models::parse_model_lock(lock_json(), "lock.json");
  expect(parsed.models.size() == 1, "model lock entry was not parsed");
  expect(parsed.models[0].files.size() == 1,
         "model lock file inventory was not parsed");

  auto missing = lock_json();
  missing["models"][0].erase("files");
  expect_model_error(
      [&] { static_cast<void>(svp::models::parse_model_lock(missing, "lock.json")); },
      "model lock without file inventory was accepted");
}

void test_duplicate_identities_are_rejected() {
  auto duplicate_model = lock_json();
  duplicate_model["models"].push_back(duplicate_model["models"][0]);
  expect_model_error(
      [&] {
        static_cast<void>(
            svp::models::parse_model_lock(duplicate_model, "lock.json"));
      },
      "duplicate model lock identity was accepted");

  auto duplicate_file = lock_json();
  duplicate_file["models"][0]["files"].push_back(
      duplicate_file["models"][0]["files"][0]);
  expect_model_error(
      [&] {
        static_cast<void>(
            svp::models::parse_model_lock(duplicate_file, "lock.json"));
      },
      "duplicate model lock file path was accepted");
}

void test_lock_rejects_file_path_role_and_hash_disagreement() {
  TemporaryDirectory temporary;
  const auto manifest = write_valid_bundle(
      temporary.path / "model_lock_test", "model_lock_test", "1.0");

  auto path_lock = lock_for_manifest(manifest);
  path_lock.models[0].files[0].path = "renamed.onnx";
  const auto path_report =
      svp::models::verify_lock_against_cache(path_lock, temporary.path);
  expect(has_error_containing(path_report, "file identity disagrees"),
         "model lock file path disagreement was accepted");

  auto role_lock = lock_for_manifest(manifest);
  role_lock.models[0].files[0].role = "weights";
  const auto role_report =
      svp::models::verify_lock_against_cache(role_lock, temporary.path);
  expect(has_error_containing(role_report, "file identity disagrees"),
         "model lock file role disagreement was accepted");

  auto hash_lock = lock_for_manifest(manifest);
  hash_lock.models[0].files[0].blake3 =
      *svp::core::parse_hash_string(std::string(kDigest));
  const auto hash_report =
      svp::models::verify_lock_against_cache(hash_lock, temporary.path);
  expect(has_error_containing(hash_report, "file identity disagrees"),
         "model lock file hash disagreement was accepted");
}

void test_lock_rejects_bundle_identity_disagreement() {
  TemporaryDirectory temporary;
  const auto manifest = write_valid_bundle(
      temporary.path / "model_lock_test", "model_lock_test", "1.0");
  auto lock = lock_for_manifest(manifest);
  lock.models[0].model_id = "model_wrong_identity";
  const auto report = svp::models::verify_lock_against_cache(lock, temporary.path);
  expect(has_error_containing(report, "model_id disagrees"),
         "model lock bundle identity disagreement was accepted");
}

void test_lock_rejects_missing_and_unexpected_bundles() {
  TemporaryDirectory temporary;
  const auto expected = write_valid_bundle(
      temporary.path / "model_expected", "model_expected", "1.0");
  auto lock = lock_for_manifest(expected);

  const auto unexpected = write_valid_bundle(
      temporary.path / "model_unexpected", "model_unexpected", "1.0");
  const auto unexpected_report =
      svp::models::verify_lock_against_cache(lock, temporary.path);
  expect(has_error_containing(unexpected_report, "unexpected model bundle"),
         "unexpected installed model bundle was accepted");

  auto missing_lock = lock_for_manifest(unexpected);
  fs::remove_all(temporary.path / "model_unexpected");
  const auto missing_report =
      svp::models::verify_lock_against_cache(missing_lock, temporary.path);
  expect(has_error_containing(missing_report, "missing model bundle"),
         "missing locked model bundle was accepted");
}

void test_lock_rejects_duplicate_installed_bundle_identity() {
  TemporaryDirectory temporary;
  const auto manifest = write_valid_bundle(
      temporary.path / "first", "model_lock_test", "1.0");
  fs::copy(temporary.path / "first", temporary.path / "second",
           fs::copy_options::recursive);
  const auto report = svp::models::verify_lock_against_cache(
      lock_for_manifest(manifest), temporary.path);
  expect(has_error_containing(report, "duplicate installed model_bundle_id"),
         "duplicate installed bundle identity was accepted");
}

}  // namespace

int main() {
  test_complete_file_inventory_is_required();
  test_duplicate_identities_are_rejected();
  test_lock_rejects_file_path_role_and_hash_disagreement();
  test_lock_rejects_bundle_identity_disagreement();
  test_lock_rejects_missing_and_unexpected_bundles();
  test_lock_rejects_duplicate_installed_bundle_identity();
  std::cout << "svp-models model lock tests: PASS\n";
  return 0;
}
