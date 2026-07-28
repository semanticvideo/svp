#include "svp/models/error.hpp"
#include "svp/models/model_lock.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::string_view kDigest =
    "blake3:1111111111111111111111111111111111111111111111111111111111111111";

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

}  // namespace

int main() {
  test_complete_file_inventory_is_required();
  test_duplicate_identities_are_rejected();
  std::cout << "svp-models model lock tests: PASS\n";
  return 0;
}
