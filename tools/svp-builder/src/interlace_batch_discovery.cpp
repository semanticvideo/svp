#include "interlace_batch_internal.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace svp::builder {

namespace {

bool is_supported_video(const std::filesystem::path& path) {
  const auto ext = path.extension().string();
  std::string ext_lower;
  ext_lower.reserve(ext.size());
  for (char c : ext) {
    ext_lower.push_back(static_cast<char>(std::tolower(c)));
  }
  for (auto supported : kSupportedVideoExts) {
    if (ext_lower == supported) return true;
  }
  return false;
}

}  // namespace

std::string make_utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::vector<std::filesystem::path> discover_media_files(
    const std::filesystem::path& dir, bool recursive) {
  std::vector<std::filesystem::path> result;
  if (recursive) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && is_supported_video(entry.path())) {
        result.push_back(entry.path());
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file() && is_supported_video(entry.path())) {
        result.push_back(entry.path());
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<std::filesystem::path> discover_svpi_files(
    const std::filesystem::path& dir, bool recursive) {
  std::vector<std::filesystem::path> result;
  auto collect = [&](const std::filesystem::path& path) {
    if (path.extension() == ".svpi") {
      result.push_back(path);
    }
  };
  if (recursive) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        collect(entry.path());
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file()) {
        collect(entry.path());
      }
    }
    auto managed = dir / ".svpi";
    if (std::filesystem::exists(managed) && std::filesystem::is_directory(managed)) {
      for (const auto& entry : std::filesystem::directory_iterator(managed)) {
        if (entry.is_regular_file()) {
          collect(entry.path());
        }
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::filesystem::path find_candidate_sidecar(
    const std::filesystem::path& media_path,
    const std::filesystem::path& search_dir) {
  std::string stem = media_path.stem().string();
  std::vector<std::filesystem::path> candidates = {
    search_dir / (stem + ".svpi"),
    search_dir / ("." + stem + ".svpi"),
    search_dir / ".svpi" / (stem + ".svpi"),
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return {};
}

std::filesystem::path media_search_dir_for_svpi(
    const std::filesystem::path& svpi_path) {
  auto parent = svpi_path.parent_path();
  if (parent.filename() == ".svpi") {
    return parent.parent_path();
  }
  return parent;
}

std::string sidecar_stem(const std::filesystem::path& svpi_path) {
  std::string stem = svpi_path.stem().string();
  if (!stem.empty() && stem[0] == '.') {
    stem = stem.substr(1);
  }
  return stem;
}

}  // namespace svp::builder
