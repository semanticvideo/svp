#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace svp::query {

struct LayerInfo {
  std::string section;
  std::string entry;
  std::string kind;  // "json", "jsonl", or "text"
  bool present = false;
  bool has_malformed = false;
  std::uint64_t record_count = 0;
  std::uint64_t malformed_line_count = 0;
  std::string error_message;
};

struct TranscriptSummaryResult {
  bool present = false;
  bool parsed = false;
  nlohmann::json transcript_json;
  std::uint64_t word_count_file = 0;
  std::uint64_t speaker_count_file = 0;
  std::uint64_t speech_region_count = 0;
  std::string error_message;
};

struct WordMatch {
  nlohmann::json record;
};

struct SpeakerInfo {
  nlohmann::json record;
  std::uint64_t word_count = 0;
};

struct OcrObservationInfo {
  nlohmann::json record;
};

struct ColorObservationInfo {
  nlohmann::json record;
};

struct ValidationInfo {
  bool present = false;
  bool parsed = false;
  nlohmann::json record;
  std::string error_message;
};

struct PackageLayerSummary {
  std::vector<LayerInfo> layers;
  std::uint64_t total_entries = 0;
  std::string error_message;
};

[[nodiscard]] PackageLayerSummary list_layers(const std::filesystem::path& package_path);

[[nodiscard]] TranscriptSummaryResult transcript_summary(
    const std::filesystem::path& package_path);

[[nodiscard]] std::vector<WordMatch> find_words(const std::filesystem::path& package_path,
                                                 const std::string& search_text,
                                                 std::size_t max_results);

[[nodiscard]] std::vector<SpeakerInfo> list_speakers(
    const std::filesystem::path& package_path);

[[nodiscard]] std::vector<OcrObservationInfo> list_ocr_observations(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& text_filter,
    std::size_t max_results);

[[nodiscard]] std::vector<ColorObservationInfo> list_color_observations(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& dominant_bucket_filter,
    std::optional<double> min_coverage_threshold,
    std::size_t max_results);

[[nodiscard]] ValidationInfo show_validation(const std::filesystem::path& package_path);

}  // namespace svp::query
