#include "canonical_cbor.hpp"
#include "canonical_control_json.hpp"
#include "svp/models/error.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/verification.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using svp::models::blake3_hex_for_file;
using svp::models::blake3_hex_for_model_bundle;
using svp::models::verify_extracted_bundle;

constexpr std::string_view kModelId = "model_digest_test";
constexpr std::string_view kModelVersion = "1.0.0";
constexpr std::string_view kZeroDigest =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kCanonicalFixtureDigest =
    "2114756c26b9db38cb522a25c42bd88dd9f130411d3310e5fc73fe4eba33c351";

struct TemporaryDirectory {
  fs::path path;

  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("svp-model-bundle-digest-tests-" + std::to_string(nonce));
    fs::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
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

void write_bytes(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  expect(static_cast<bool>(output), "could not open test fixture file");
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  expect(static_cast<bool>(output), "could not write test fixture file");
}

void write_json(const fs::path& path, const nlohmann::ordered_json& value) {
  write_bytes(path, value.dump());
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  expect(static_cast<bool>(input), "could not open test fixture text");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void replace_once(std::string& text,
                  std::string_view original,
                  std::string_view replacement) {
  const std::size_t offset = text.find(original);
  expect(offset != std::string::npos, "test fixture token was not found");
  text.replace(offset, original.size(), replacement);
}

std::string canonical_hash(std::string_view hex) {
  return "blake3:" + std::string(hex);
}

std::string bundle_id(std::string_view hex) {
  return std::string(kModelId) + "@" + std::string(kModelVersion) +
         "+blake3_" + std::string(hex.substr(0, 12));
}

nlohmann::ordered_json manifest_json(const fs::path& root,
                                     std::string_view digest_hex,
                                     bool reverse_object_insertion) {
  nlohmann::ordered_json manifest;
  if (reverse_object_insertion) {
    manifest["postprocessor_contract"] = {{"threshold", 0.5}, {"enabled", true}};
    manifest["preprocessor_contract"] = {{"height", 224}, {"width", 224}};
    manifest["output_contract"] = {{"shape", {1, 2}}};
    manifest["input_contract"] = {{"shape", {1, 3, 224, 224}}};
  }
  manifest["schema_version"] = "svp-model-bundle-1";
  manifest["model_bundle_id"] = bundle_id(digest_hex);
  manifest["model_id"] = kModelId;
  manifest["model_version"] = kModelVersion;
  manifest["bundle_blake3"] = canonical_hash(digest_hex);
  manifest["display_name"] = "Digest Test";
  manifest["source_registry"] = "test";
  manifest["source_slug"] = "semanticvideo/digest-test";
  manifest["source_revision"] = "immutable-test-revision";
  manifest["runtime"] = "native";
  manifest["format"] = "native";
  manifest["license"] = "Apache-2.0";
  manifest["supported_execution_providers"] = {"cpu"};
  manifest["files"] = nlohmann::ordered_json::array(
      {{{"path", "assets/caf\u00e9.txt"},
        {"role", "empty_unicode_fixture"},
        {"blake3", canonical_hash(blake3_hex_for_file(root / "assets/caf\u00e9.txt"))}},
       {{"path", "weights/model.bin"},
        {"role", "weights"},
        {"blake3", canonical_hash(blake3_hex_for_file(root / "weights/model.bin"))}},
       {{"path", "LICENSE"},
        {"role", "license"},
        {"blake3", canonical_hash(blake3_hex_for_file(root / "LICENSE"))}},
       {{"path", "NOTICE"},
        {"role", "notice"},
        {"blake3", canonical_hash(blake3_hex_for_file(root / "NOTICE"))}}});
  if (!reverse_object_insertion) {
    manifest["input_contract"] = {{"shape", {1, 3, 224, 224}}};
    manifest["output_contract"] = {{"shape", {1, 2}}};
    manifest["preprocessor_contract"] = {{"width", 224}, {"height", 224}};
    manifest["postprocessor_contract"] = {{"enabled", true}, {"threshold", 0.5}};
  }
  return manifest;
}

std::string create_bundle(const fs::path& root,
                          bool reverse_file_creation,
                          bool reverse_object_insertion) {
  fs::create_directories(root);
  if (reverse_file_creation) {
    write_bytes(root / "weights/model.bin",
                std::string_view("model-weights\0with-binary", 25));
    write_bytes(root / "assets/caf\u00e9.txt", "");
    write_bytes(root / "NOTICE", "SVP digest test notice\n");
    write_bytes(root / "LICENSE", "SVP digest test license\n");
  } else {
    write_bytes(root / "LICENSE", "SVP digest test license\n");
    write_bytes(root / "NOTICE", "SVP digest test notice\n");
    write_bytes(root / "assets/caf\u00e9.txt", "");
    write_bytes(root / "weights/model.bin",
                std::string_view("model-weights\0with-binary", 25));
  }

  write_json(root / "model.svpmodel.json",
             manifest_json(root, kZeroDigest, reverse_object_insertion));
  const std::string digest = blake3_hex_for_model_bundle(root);
  write_json(root / "model.svpmodel.json",
             manifest_json(root, digest, reverse_object_insertion));
  expect(blake3_hex_for_model_bundle(root) == digest,
         "recursive digest projection was not stable");
  return digest;
}

void test_deterministic_bundle_digest() {
  TemporaryDirectory temporary;
  const fs::path first = temporary.path / "first";
  const fs::path second = temporary.path / "second";
  const std::string first_digest = create_bundle(first, false, false);
  const std::string second_digest = create_bundle(second, true, true);

  expect(first_digest == second_digest,
         "creation and object insertion order changed the digest");
  std::cout << "canonical digest: " << first_digest << '\n';
  expect(first_digest == kCanonicalFixtureDigest,
         "canonical digest changed from the cross-implementation test vector");
  expect(first_digest.size() == 64, "bundle digest was not 64 hex characters");
  expect(verify_extracted_bundle(first).ok(), "first bundle did not verify");
  expect(verify_extracted_bundle(second).ok(), "second bundle did not verify");
  const auto report = verify_extracted_bundle(first);
  expect(std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
           return issue.severity == svp::models::VerificationSeverity::info &&
                  issue.message.find("bundle BLAKE3") != std::string::npos;
         }),
         "bundle verification omitted aggregate digest proof");
}

void test_recursive_fields_are_projected() {
  TemporaryDirectory temporary;
  const fs::path root = temporary.path / "bundle";
  const std::string digest = create_bundle(root, false, false);

  const std::string alternate(64, 'a');
  write_json(root / "model.svpmodel.json", manifest_json(root, alternate, false));
  expect(blake3_hex_for_model_bundle(root) == digest,
         "recursive manifest fields changed the digest");
}

void test_covered_mutations_change_digest() {
  TemporaryDirectory temporary;
  const fs::path changed = temporary.path / "changed";
  const fs::path renamed = temporary.path / "renamed";
  const fs::path missing = temporary.path / "missing";
  const fs::path extra = temporary.path / "extra";
  const fs::path metadata = temporary.path / "metadata";
  const std::string original = create_bundle(changed, false, false);
  const std::string renamed_digest = create_bundle(renamed, false, false);
  const std::string missing_digest = create_bundle(missing, false, false);
  const std::string extra_digest = create_bundle(extra, false, false);
  const std::string metadata_digest = create_bundle(metadata, false, false);
  expect(renamed_digest == original && missing_digest == original &&
             extra_digest == original && metadata_digest == original,
         "equivalent fixture bundles produced different digests");

  write_bytes(changed / "weights/model.bin", "changed");
  expect(blake3_hex_for_model_bundle(changed) != original,
         "changed file bytes did not change the digest");
  expect(!verify_extracted_bundle(changed).ok(),
         "changed file bytes passed bundle verification");

  fs::rename(renamed / "weights/model.bin", renamed / "weights/renamed.bin");
  expect_model_error(
      [&] { static_cast<void>(blake3_hex_for_model_bundle(renamed)); },
      "renamed file was not rejected as an inventory mismatch");
  expect(!verify_extracted_bundle(renamed).ok(),
         "renamed file passed bundle verification");

  fs::remove(missing / "weights/model.bin");
  expect_model_error(
      [&] { static_cast<void>(blake3_hex_for_model_bundle(missing)); },
      "missing file was not rejected as an inventory mismatch");
  expect(!verify_extracted_bundle(missing).ok(),
         "missing file passed bundle verification");

  write_bytes(extra / "unexpected.bin", "extra");
  expect_model_error(
      [&] { static_cast<void>(blake3_hex_for_model_bundle(extra)); },
      "undeclared extra file was not rejected");
  expect(!verify_extracted_bundle(extra).ok(),
         "extra file passed bundle verification");

  auto manifest = manifest_json(metadata, original, false);
  manifest["license"] = "CC-BY-4.0";
  write_json(metadata / "model.svpmodel.json", manifest);
  expect(blake3_hex_for_model_bundle(metadata) != original,
         "covered manifest metadata did not change the digest");
  expect(!verify_extracted_bundle(metadata).ok(),
         "covered manifest metadata passed bundle verification");
}

void test_json_number_canonicalization() {
  TemporaryDirectory temporary;
  const fs::path root = temporary.path / "bundle";
  const std::string baseline = create_bundle(root, false, false);
  const std::string manifest = read_text(root / "model.svpmodel.json");

  auto digest_with = [&](std::string_view number) {
    std::string changed = manifest;
    replace_once(changed, "\"threshold\":0.5",
                 "\"threshold\":" + std::string(number));
    write_bytes(root / "model.svpmodel.json", changed);
    return blake3_hex_for_model_bundle(root);
  };
  expect(digest_with("5e-1") == baseline && digest_with("0.50") == baseline,
         "equivalent fractional JSON spellings changed the digest");
  const std::string one = digest_with("1");
  expect(digest_with("1.0") == one && digest_with("1e0") == one,
         "equivalent integral JSON spellings changed the digest");
  const std::string zero = digest_with("-0");
  expect(digest_with("-0.0") == zero && digest_with("0e99") == zero,
         "equivalent zero JSON spellings changed the digest");

  expect_model_error([&] { static_cast<void>(digest_with("18446744073709551616")); },
                     "out-of-range unsigned integer was accepted");
  expect_model_error([&] { static_cast<void>(digest_with("-9223372036854775809")); },
                     "out-of-range signed integer was accepted");
  expect_model_error([&] { static_cast<void>(digest_with("1e-4000")); },
                     "binary64 underflow was accepted");
}

std::vector<std::uint8_t> projected_bytes(std::string_view json) {
  const std::vector<std::uint8_t> input(json.begin(), json.end());
  return svp::models::detail::encode_canonical_control_cbor(
      svp::models::detail::parse_canonical_control_json(input, "number.json"));
}

void test_exact_numeric_cbor_vectors() {
  const std::vector<std::uint8_t> one = {0xa1, 0x61, 0x6e, 0x01};
  expect(projected_bytes("{\"n\":1}") == one &&
             projected_bytes("{\"n\":1.0}") == one &&
             projected_bytes("{\"n\":1e0}") == one,
         "integral CBOR vector did not match the contract");

  const std::vector<std::uint8_t> zero = {0xa1, 0x61, 0x6e, 0x00};
  expect(projected_bytes("{\"n\":-0}") == zero &&
             projected_bytes("{\"n\":-0.0}") == zero,
         "zero CBOR vector did not match the contract");

  const std::vector<std::uint8_t> half = {
      0xa1, 0x61, 0x6e, 0xfb, 0x3f, 0xe0, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00};
  expect(projected_bytes("{\"n\":0.5}") == half &&
             projected_bytes("{\"n\":5e-1}") == half,
         "binary64 CBOR vector did not match the contract");
}

void test_duplicate_object_names_rejected() {
  TemporaryDirectory temporary;
  const fs::path root = temporary.path / "bundle";
  create_bundle(root, false, false);
  std::string manifest = read_text(root / "model.svpmodel.json");
  replace_once(manifest, "\"threshold\":0.5",
               "\"threshold\":0.5,\"threshold\":0.5");
  write_bytes(root / "model.svpmodel.json", manifest);
  expect_model_error(
      [&] { static_cast<void>(blake3_hex_for_model_bundle(root)); },
      "duplicate nested object name was accepted");
}

void test_unsupported_file_type_rejected() {
  TemporaryDirectory temporary;
  const fs::path root = temporary.path / "bundle";
  create_bundle(root, false, false);
  std::error_code error;
  fs::create_symlink(root / "weights/model.bin", root / "link", error);
  if (error) {
    return;
  }
  expect_model_error([&] { static_cast<void>(blake3_hex_for_model_bundle(root)); },
                     "symbolic link was not rejected");
}

void test_noncanonical_path_rejected() {
  TemporaryDirectory temporary;
  const fs::path root = temporary.path / "bundle";
  create_bundle(root, false, false);
  auto manifest = manifest_json(root, kZeroDigest, false);
  manifest["files"][0]["path"] = "bad\\name";
  write_json(root / "model.svpmodel.json", manifest);
  expect_model_error([&] { static_cast<void>(blake3_hex_for_model_bundle(root)); },
                     "backslash logical path was not rejected");
}

void test_fixed_root_manifest_declarations() {
  TemporaryDirectory temporary;
  const fs::path duplicate_root = temporary.path / "duplicate";
  const fs::path role_root = temporary.path / "role";
  create_bundle(duplicate_root, false, false);
  create_bundle(role_root, false, false);

  auto duplicate = manifest_json(duplicate_root, kZeroDigest, false);
  duplicate["files"].push_back(duplicate["files"][2]);
  write_json(duplicate_root / "model.svpmodel.json", duplicate);
  expect_model_error(
      [&] { static_cast<void>(blake3_hex_for_model_bundle(duplicate_root)); },
      "duplicate LICENSE declaration was accepted");
  expect(!verify_extracted_bundle(duplicate_root).ok(),
         "duplicate LICENSE declaration passed public verification");

  auto conflicting_role = manifest_json(role_root, kZeroDigest, false);
  conflicting_role["files"][2]["role"] = "notice";
  write_json(role_root / "model.svpmodel.json", conflicting_role);
  expect_model_error(
      [&] { static_cast<void>(blake3_hex_for_model_bundle(role_root)); },
      "LICENSE declaration with notice role was accepted");
  expect(!verify_extracted_bundle(role_root).ok(),
         "LICENSE role/path conflict passed public verification");
}

std::string create_unicode_path_bundle(const fs::path& root,
                                       std::string_view logical_path) {
  create_bundle(root, false, false);
  auto manifest = manifest_json(root, kZeroDigest, false);
  fs::remove(root / "assets/caf\u00e9.txt");
  const fs::path physical = root / fs::path(std::u8string(
      reinterpret_cast<const char8_t*>(logical_path.data()), logical_path.size()));
  write_bytes(physical, "unicode");
  manifest["files"][0]["path"] = std::string(logical_path);
  manifest["files"][0]["blake3"] = canonical_hash(blake3_hex_for_file(physical));
  write_json(root / "model.svpmodel.json", manifest);
  const std::string digest = blake3_hex_for_model_bundle(root);
  manifest["bundle_blake3"] = canonical_hash(digest);
  manifest["model_bundle_id"] = bundle_id(digest);
  write_json(root / "model.svpmodel.json", manifest);
  return digest;
}

void test_unicode_logical_path_identity_and_aliases() {
  constexpr std::string_view nfc = "assets/caf\u00e9.txt";
  constexpr std::string_view nfd = "assets/cafe\u0301.txt";
  TemporaryDirectory temporary;
  const fs::path nfc_root = temporary.path / "nfc";
  const fs::path nfd_root = temporary.path / "nfd";
  expect(create_unicode_path_bundle(nfc_root, nfc) !=
             create_unicode_path_bundle(nfd_root, nfd),
         "distinct Unicode logical paths produced the same digest");

  auto manifest = manifest_json(nfc_root, kZeroDigest, false);
  manifest["files"].push_back(manifest["files"][0]);
  write_json(nfc_root / "model.svpmodel.json", manifest);
  expect_model_error([&] { static_cast<void>(blake3_hex_for_model_bundle(nfc_root)); },
                     "duplicate logical path was accepted");

  std::error_code error;
  const fs::path nfc_physical = nfc_root / fs::path(std::u8string(
      reinterpret_cast<const char8_t*>(nfc.data()), nfc.size()));
  const fs::path nfd_alias = nfc_root / fs::path(std::u8string(
      reinterpret_cast<const char8_t*>(nfd.data()), nfd.size()));
  if (fs::equivalent(nfc_physical, nfd_alias, error) && !error) {
    manifest = manifest_json(nfc_root, kZeroDigest, false);
    auto alias = manifest["files"][0];
    alias["path"] = std::string(nfd);
    manifest["files"].push_back(alias);
    write_json(nfc_root / "model.svpmodel.json", manifest);
    expect_model_error(
        [&] { static_cast<void>(blake3_hex_for_model_bundle(nfc_root)); },
        "Unicode filesystem alias collision was accepted");
  }
}

}  // namespace

int main() {
  test_deterministic_bundle_digest();
  test_recursive_fields_are_projected();
  test_covered_mutations_change_digest();
  test_json_number_canonicalization();
  test_exact_numeric_cbor_vectors();
  test_duplicate_object_names_rejected();
  test_unsupported_file_type_rejected();
  test_noncanonical_path_rejected();
  test_fixed_root_manifest_declarations();
  test_unicode_logical_path_identity_and_aliases();
  std::cout << "svp-models bundle digest tests: PASS\n";
  return 0;
}
