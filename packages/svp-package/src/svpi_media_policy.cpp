#include "svp/package/svpi_media_policy.hpp"

#include <algorithm>
#include <string>

namespace svp::package {
namespace {

bool starts_with(std::string_view str, std::string_view prefix) {
  return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view str, std::string_view suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string to_lower(std::string_view sv) {
  std::string s{sv};
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool is_replayable_audio_extension(std::string_view path) {
  const std::string lower = to_lower(path);
  return ends_with(lower, ".flac") ||
         ends_with(lower, ".wav") ||
         ends_with(lower, ".mp3") ||
         ends_with(lower, ".aac") ||
         ends_with(lower, ".ogg") ||
         ends_with(lower, ".opus") ||
         ends_with(lower, ".m4a") ||
         ends_with(lower, ".wma") ||
         ends_with(lower, ".aiff") ||
         ends_with(lower, ".alac");
}

bool is_replayable_video_extension(std::string_view path) {
  const std::string lower = to_lower(path);
  return ends_with(lower, ".mp4") ||
         ends_with(lower, ".mov") ||
         ends_with(lower, ".mkv") ||
         ends_with(lower, ".avi") ||
         ends_with(lower, ".webm") ||
         ends_with(lower, ".wmv") ||
         ends_with(lower, ".flv") ||
         ends_with(lower, ".m4v") ||
         ends_with(lower, ".mpg") ||
         ends_with(lower, ".mpeg") ||
         ends_with(lower, ".ts") ||
         ends_with(lower, ".3gp") ||
         ends_with(lower, ".3g2") ||
         ends_with(lower, ".vob");
}

bool is_known_non_replayable_artifact(std::string_view path) {
  const std::string lower = to_lower(path);
  if (ends_with(lower, "waveform.jsonl") ||
      ends_with(lower, "audio_absence.json")) {
    return true;
  }
  if (starts_with(lower, "text/evidence_crops/") && ends_with(lower, ".jpg")) {
    return true;
  }
  if (ends_with(lower, ".jsonl") || ends_with(lower, ".json") ||
      ends_with(lower, ".svpdz") || ends_with(lower, ".svpmz") ||
      ends_with(lower, ".svpez") || ends_with(lower, ".sqlite")) {
    return true;
  }
  return false;
}

}  // namespace

SvpiForbiddenMediaReason
classify_svpi_entry(std::string_view entry_path) noexcept {
  if (starts_with(entry_path, "media/original/") ||
      entry_path == "media/original") {
    return SvpiForbiddenMediaReason::primary_media;
  }

  if (is_known_non_replayable_artifact(entry_path)) {
    return SvpiForbiddenMediaReason::none;
  }

  if (is_replayable_audio_extension(entry_path) ||
      is_replayable_video_extension(entry_path)) {
    return SvpiForbiddenMediaReason::replayable_derivative;
  }

  return SvpiForbiddenMediaReason::none;
}

bool is_svpi_entry_forbidden(std::string_view entry_path) noexcept {
  return classify_svpi_entry(entry_path) != SvpiForbiddenMediaReason::none;
}

}  // namespace svp::package
