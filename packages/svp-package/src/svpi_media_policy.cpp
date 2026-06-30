#include "svp/package/svpi_media_policy.hpp"

namespace svp::package {
namespace {

bool starts_with(std::string_view str, std::string_view prefix) {
  return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view str, std::string_view suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_replayable_audio_extension(std::string_view path) {
  return ends_with(path, ".flac") ||
         ends_with(path, ".wav") ||
         ends_with(path, ".mp3") ||
         ends_with(path, ".aac") ||
         ends_with(path, ".ogg") ||
         ends_with(path, ".opus") ||
         ends_with(path, ".m4a") ||
         ends_with(path, ".wma") ||
         ends_with(path, ".aiff") ||
         ends_with(path, ".alac");
}

bool is_replayable_video_extension(std::string_view path) {
  return ends_with(path, ".mp4") ||
         ends_with(path, ".mov") ||
         ends_with(path, ".mkv") ||
         ends_with(path, ".avi") ||
         ends_with(path, ".webm") ||
         ends_with(path, ".wmv") ||
         ends_with(path, ".flv") ||
         ends_with(path, ".m4v") ||
         ends_with(path, ".mpg") ||
         ends_with(path, ".mpeg") ||
         ends_with(path, ".ts") ||
         ends_with(path, ".3gp") ||
         ends_with(path, ".3g2") ||
         ends_with(path, ".vob");
}

bool is_known_non_replayable_artifact(std::string_view path) {
  if (ends_with(path, "waveform.jsonl") ||
      ends_with(path, "audio_absence.json")) {
    return true;
  }
  if (starts_with(path, "text/evidence_crops/") && ends_with(path, ".jpg")) {
    return true;
  }
  if (ends_with(path, ".jsonl") || ends_with(path, ".json") ||
      ends_with(path, ".svpdz") || ends_with(path, ".svpmz") ||
      ends_with(path, ".svpez") || ends_with(path, ".sqlite")) {
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
