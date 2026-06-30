#include "svp/package/svpi_media_policy.hpp"

namespace svp::package {
namespace {

bool starts_with(std::string_view str, std::string_view prefix) {
  return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

SvpiForbiddenMediaReason
classify_svpi_entry(std::string_view entry_path) noexcept {
  if (starts_with(entry_path, "media/original/") ||
      entry_path == "media/original") {
    return SvpiForbiddenMediaReason::primary_media;
  }

  if (starts_with(entry_path, "media/audio/original_stream_") ||
      entry_path == "media/audio/analysis_mono_16k.wav") {
    return SvpiForbiddenMediaReason::replayable_derivative;
  }

  return SvpiForbiddenMediaReason::none;
}

bool is_svpi_entry_forbidden(std::string_view entry_path) noexcept {
  return classify_svpi_entry(entry_path) != SvpiForbiddenMediaReason::none;
}

}  // namespace svp::package
