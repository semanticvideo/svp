#include "cli_context.hpp"
#include "cli_completion.hpp"

#include "svp/builder/embedded_svpi_transport.hpp"

#include <iostream>
#include <chrono>

int run_transport_command(const TransportCliOptions& options) {
  if (*options.embed_sub) {
    const auto started_at = std::chrono::steady_clock::now();
    svp::builder::EmbeddedTransportEmbedOptions embed;
    embed.container_path = options.embed_container;
    embed.svpi_path = options.embed_svpi;
    embed.output_path = options.embed_out;
    embed.ffprobe_path = options.embed_ffprobe;
    embed.validation_codes_path = options.embed_codes;
    embed.replace_existing = options.embed_replace;
    embed.overwrite_output = options.embed_overwrite;
    const auto result = svp::builder::embed_svpi_transport(embed);
    if (!result.success) {
      std::cerr << "transport embed failed: " << result.error_message << "\n";
      for (const auto& finding : result.validation_report.errors) {
        std::cerr << "  " << finding.code << ": " << finding.message << "\n";
      }
      return 1;
    }
    std::cout << format_cli_completion(
                     kEmbeddedSvpiTransportArtifactLabel, "created",
                     result.output_path,
                     std::chrono::steady_clock::now() - started_at)
              << "\n";
    return 0;
  }

  if (*options.extract_sub) {
    const auto started_at = std::chrono::steady_clock::now();
    svp::builder::EmbeddedTransportExtractOptions extract;
    extract.container_path = options.extract_container;
    extract.output_path = options.extract_out;
    extract.validation_codes_path = options.extract_codes;
    extract.overwrite_output = options.extract_overwrite;
    const auto result = svp::builder::extract_svpi_transport(extract);
    if (!result.success) {
      std::cerr << "transport extract failed: " << result.error_message << "\n";
      return 1;
    }
    std::cout << "Embedded package validation: "
              << (result.package_valid ? "valid" : "invalid") << "\n";
    if (!result.package_valid) {
      std::cerr << result.error_message << "\n";
      return 1;
    }
    std::cout << format_cli_completion(
                     kSvpiArtifactLabel, "extracted", result.output_path,
                     std::chrono::steady_clock::now() - started_at)
              << "\n";
    return 0;
  }

  if (*options.strip_sub) {
    const auto started_at = std::chrono::steady_clock::now();
    svp::builder::EmbeddedTransportStripOptions strip;
    strip.container_path = options.strip_container;
    strip.output_path = options.strip_out;
    strip.overwrite_output = options.strip_overwrite;
    const auto result = svp::builder::strip_svpi_transport(strip);
    if (!result.success) {
      std::cerr << "transport strip failed: " << result.error_message << "\n";
      return 1;
    }
    std::cout << format_cli_completion(
                     kCleanIsoBmffContainerArtifactLabel, "created",
                     result.output_path,
                     std::chrono::steady_clock::now() - started_at)
              << "\n";
    return 0;
  }

  return 0;
}
