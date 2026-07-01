#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder {

inline constexpr std::string_view kSupportedVideoExts[] = {
    ".mov", ".mp4", ".mkv", ".avi", ".webm", ".m4v", ".wmv", ".flv"
};

std::string make_utc_timestamp();

std::vector<std::filesystem::path> discover_media_files(
    const std::filesystem::path& dir, bool recursive);

std::vector<std::filesystem::path> discover_svpi_files(
    const std::filesystem::path& dir, bool recursive);

std::filesystem::path find_candidate_sidecar(
    const std::filesystem::path& media_path,
    const std::filesystem::path& search_dir);

std::filesystem::path media_search_dir_for_svpi(
    const std::filesystem::path& svpi_path);

std::string sidecar_stem(const std::filesystem::path& svpi_path);

}  // namespace svp::builder
