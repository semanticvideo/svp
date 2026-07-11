#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

inline constexpr std::string_view kSvpArtifactLabel = "SVP";
inline constexpr std::string_view kSvpiArtifactLabel = "SVPI";
inline constexpr std::string_view kEmbeddedSvpiTransportArtifactLabel =
    "Embedded SVPI transport";
inline constexpr std::string_view kCleanIsoBmffContainerArtifactLabel =
    "Clean ISO BMFF container";
inline constexpr std::string_view kInterlaceArtifactsLabel =
    "Interlace artifacts";

std::string_view build_artifact_label(std::string_view output_format);

std::string format_cli_completion(
    std::string_view subject,
    std::string_view action,
    const std::filesystem::path& output_path,
    std::chrono::steady_clock::duration elapsed);
