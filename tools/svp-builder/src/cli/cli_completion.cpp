#include "cli_completion.hpp"

#include "cli_elapsed.hpp"

#include <stdexcept>

std::string_view build_artifact_label(std::string_view output_format) {
  if (output_format == "svp") return kSvpArtifactLabel;
  if (output_format == "svpi") return kSvpiArtifactLabel;
  if (output_format == "embedded-svpi") {
    return kEmbeddedSvpiTransportArtifactLabel;
  }
  throw std::invalid_argument("unsupported build output format");
}

std::string format_cli_completion(
    std::string_view subject,
    std::string_view action,
    const std::filesystem::path& output_path,
    std::chrono::steady_clock::duration elapsed) {
  return std::string(subject) + " " + std::string(action) + " in " +
         format_elapsed_duration(elapsed) + ": " + output_path.string();
}
