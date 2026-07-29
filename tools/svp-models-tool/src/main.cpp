#include "svp/models/cache.hpp"
#include "svp/models/error.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/model_lock.hpp"
#include "svp/models/reference_model_set.hpp"
#include "svp/models/verification.hpp"
#include "install_command.hpp"
#include "svp/progress/renderer.hpp"

#include <CLI/CLI.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace {

void print_report(const svp::models::VerificationReport& report) {
  for (const svp::models::VerificationIssue& issue : report.issues) {
    const char* prefix =
        issue.severity == svp::models::VerificationSeverity::error ? "ERROR" : "INFO";
    std::cout << prefix << ": " << issue.message << '\n';
  }
}

nlohmann::json load_json_for_schema_detection(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw svp::models::ModelError(svp::models::ModelErrorCode::io_error,
                                  "could not open JSON file: " + path.string());
  }

  try {
    nlohmann::json value;
    input >> value;
    return value;
  } catch (const nlohmann::json::exception& error) {
    throw svp::models::ModelError(svp::models::ModelErrorCode::schema_error,
                                  "invalid JSON in " + path.string() + ": " +
                                      error.what());
  }
}

svp::models::VerificationReport verify_model_set_path(
    const std::filesystem::path& model_set_path,
    const std::filesystem::path& cache_root) {
  const nlohmann::json value = load_json_for_schema_detection(model_set_path);
  if (!value.is_object() || !value.contains("schema_version") ||
      !value.at("schema_version").is_string()) {
    throw svp::models::ModelError(
        svp::models::ModelErrorCode::schema_error,
        model_set_path.string() + " must contain a string schema_version");
  }

  const std::string schema_version = value.at("schema_version").get<std::string>();
  if (schema_version == "svp-model-lock-1") {
    return svp::models::verify_lock_against_cache(
        svp::models::parse_model_lock(value, model_set_path.string()), cache_root);
  }

  if (schema_version == "svp-reference-model-set-1") {
    return svp::models::verify_reference_set_against_cache(
        svp::models::parse_reference_model_set(value, model_set_path.string()),
        cache_root);
  }

  throw svp::models::ModelError(
      svp::models::ModelErrorCode::schema_error,
      model_set_path.string() +
          " must be an svp-model-lock-1 or svp-reference-model-set-1 document");
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"SVP model bundle utility"};

  std::filesystem::path path_cache_dir = svp::models::model_cache_root();
  auto* path_command = app.add_subcommand("path", "Print the model cache path");
  path_command->add_option("--cache-dir", path_cache_dir,
                           "Override the model cache directory");

  std::filesystem::path install_cache_dir = svp::models::model_cache_root();
  int parallel_downloads = 1;
  std::string install_progress_mode = "auto";
  auto* install_command = app.add_subcommand(
      "install", "Install the exact SVP reference-model set");
  install_command->add_option("--cache-dir", install_cache_dir,
                              "Override the model cache directory");
  install_command
      ->add_option("--parallel-downloads", parallel_downloads,
                   "Concurrent model jobs (1 or 2)")
      ->check(CLI::Range(1, 2));
  install_command
      ->add_option("--progress", install_progress_mode,
                   "Progress output: auto, plain, json, or none")
      ->check(CLI::IsMember({"auto", "plain", "json", "none"}));

  std::filesystem::path hash_file;
  auto* hash_command =
      app.add_subcommand("hash", "Print the BLAKE3 digest of one file");
  hash_command->add_option("--file", hash_file, "File to hash")->required();

  std::filesystem::path digest_bundle_dir;
  auto* digest_command = app.add_subcommand(
      "digest", "Print the canonical BLAKE3 digest of an extracted bundle");
  digest_command
      ->add_option("--bundle-dir", digest_bundle_dir,
                   "Path to an extracted .svpmodel bundle directory")
      ->required();

  std::filesystem::path manifest_path;
  std::filesystem::path bundle_dir;
  std::filesystem::path lock_path;
  std::filesystem::path model_set_path;
  std::filesystem::path cache_dir = svp::models::model_cache_root();

  auto* verify_command = app.add_subcommand("verify", "Verify model bundle metadata");
  verify_command->add_option("--manifest", manifest_path,
                             "Path to model.svpmodel.json; verifies its complete containing bundle");
  verify_command->add_option("--bundle-dir", bundle_dir,
                             "Path to an extracted .svpmodel bundle directory");
  verify_command->add_option("--lock", lock_path, "Path to model-lock.json");
  verify_command->add_option("--model-set", model_set_path,
                             "Path to model-lock.json or reference-model-set.json");
  verify_command->add_option("--cache-dir", cache_dir,
                             "Model cache directory to inspect");

  app.require_subcommand(1);

  try {
    app.parse(argc, argv);

    if (*path_command) {
      std::cout << path_cache_dir.string() << '\n';
      return 0;
    }

    if (*install_command) {
      const auto mode = svp::progress::parse_mode(install_progress_mode);
      if (!mode) {
        std::cerr << "ERROR: unsupported progress mode\n";
        return 2;
      }
      return svp::models::tool::install_reference_models(
          argv[0], install_cache_dir, parallel_downloads, *mode,
          std::cout, isatty(STDOUT_FILENO) != 0, STDOUT_FILENO);
    }

    if (*hash_command) {
      std::cout << "blake3:" << svp::models::blake3_hex_for_file(hash_file) << '\n';
      return 0;
    }

    if (*digest_command) {
      std::cout << "blake3:"
                << svp::models::blake3_hex_for_model_bundle(digest_bundle_dir)
                << '\n';
      return 0;
    }

    if (*verify_command) {
      const int selected_inputs = static_cast<int>(!manifest_path.empty()) +
                                  static_cast<int>(!bundle_dir.empty()) +
                                  static_cast<int>(!lock_path.empty()) +
                                  static_cast<int>(!model_set_path.empty());
      if (selected_inputs != 1) {
        std::cerr << "ERROR: verify requires exactly one of --manifest, --bundle-dir, "
                     "--lock, or --model-set\n";
        return 2;
      }

      svp::models::VerificationReport report;
      if (!manifest_path.empty()) {
        if (manifest_path.filename() != "model.svpmodel.json") {
          std::cerr << "ERROR: --manifest must name model.svpmodel.json\n";
          return 2;
        }
        const std::filesystem::path root =
            manifest_path.parent_path().empty() ? std::filesystem::current_path()
                                                : manifest_path.parent_path();
        report = svp::models::verify_extracted_bundle(root);
      } else if (!bundle_dir.empty()) {
        report = svp::models::verify_extracted_bundle(bundle_dir);
      } else if (!lock_path.empty()) {
        report = svp::models::verify_lock_against_cache(
            svp::models::load_model_lock(lock_path), cache_dir);
      } else {
        report = verify_model_set_path(model_set_path, cache_dir);
      }

      print_report(report);
      return report.ok() ? 0 : 2;
    }
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  } catch (const svp::models::ModelError& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 2;
  }

  return 0;
}
