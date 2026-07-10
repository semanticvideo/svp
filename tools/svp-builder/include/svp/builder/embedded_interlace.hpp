#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/validation/report.hpp"

#include <filesystem>
#include <string>

namespace svp::builder {

struct EmbedMp4Options {
  std::filesystem::path media_path;
  std::filesystem::path svpi_path;
  std::filesystem::path output_path;
  std::filesystem::path validation_codes_path =
      "spec/registries/validation-codes.json";
  std::string ffprobe_path = "ffprobe";
  bool replace_existing = false;
  bool overwrite_output = false;
  std::shared_ptr<BuildProgressSink> progress_sink;
};

struct EmbedMp4Result {
  bool success = false;
  std::filesystem::path output_path;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] EmbedMp4Result interlace_embed_mp4(
    const EmbedMp4Options& options);

struct ExtractEmbeddedOptions {
  std::filesystem::path mp4_path;
  std::filesystem::path output_path;
  std::filesystem::path validation_codes_path =
      "spec/registries/validation-codes.json";
  bool overwrite_output = false;
};

struct ExtractEmbeddedResult {
  bool success = false;
  bool package_valid = false;
  std::filesystem::path output_path;
  svp::validation::ValidationReport validation_report;
  std::string error_message;
};

[[nodiscard]] ExtractEmbeddedResult interlace_extract_embedded(
    const ExtractEmbeddedOptions& options);

struct StripEmbeddedOptions {
  std::filesystem::path mp4_path;
  std::filesystem::path output_path;
  bool overwrite_output = false;
};

struct StripEmbeddedResult {
  bool success = false;
  std::filesystem::path output_path;
  std::string error_message;
};

[[nodiscard]] StripEmbeddedResult interlace_strip_embedded(
    const StripEmbeddedOptions& options);

}  // namespace svp::builder
