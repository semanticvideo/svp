#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/builder/interlace.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace svp::builder {

struct EmbeddedTransportBuildOptions {
  InterlaceCreateOptions svpi_options;
  std::filesystem::path output_path;
  bool overwrite_output = false;
};

struct EmbeddedTransportBuildResult {
  bool success = false;
  std::filesystem::path output_path;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] EmbeddedTransportBuildResult build_embedded_svpi_transport(
    const EmbeddedTransportBuildOptions& options);

struct EmbeddedTransportEmbedOptions {
  std::filesystem::path container_path;
  std::filesystem::path svpi_path;
  std::filesystem::path output_path;
  std::filesystem::path validation_codes_path =
      "spec/registries/validation-codes.json";
  std::string ffprobe_path = "ffprobe";
  bool replace_existing = false;
  bool overwrite_output = false;
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct EmbeddedTransportEmbedResult {
  bool success = false;
  std::filesystem::path output_path;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] EmbeddedTransportEmbedResult embed_svpi_transport(
    const EmbeddedTransportEmbedOptions& options);

struct EmbeddedTransportExtractOptions {
  std::filesystem::path container_path;
  std::filesystem::path output_path;
  std::filesystem::path validation_codes_path =
      "spec/registries/validation-codes.json";
  bool overwrite_output = false;
};

struct EmbeddedTransportExtractResult {
  bool success = false;
  bool package_valid = false;
  std::filesystem::path output_path;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] EmbeddedTransportExtractResult extract_svpi_transport(
    const EmbeddedTransportExtractOptions& options);

struct EmbeddedTransportStripOptions {
  std::filesystem::path container_path;
  std::filesystem::path output_path;
  bool overwrite_output = false;
};

struct EmbeddedTransportStripResult {
  bool success = false;
  std::filesystem::path output_path;
  std::string error_message;
};

[[nodiscard]] EmbeddedTransportStripResult strip_svpi_transport(
    const EmbeddedTransportStripOptions& options);

}  // namespace svp::builder
