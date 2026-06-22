#include "diarization_validation.hpp"

#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace svp::validation {
namespace {

bool has_entry(const svp::package::PackageLayout& layout, const std::string& entry) {
  return layout.has_entry(entry);
}

void add_diarization_findings_impl(ValidationReport& report,
                                   const ValidationCodeRegistry& registry,
                                   const std::filesystem::path& package_path,
                                   const svp::package::PackageLayout& layout) {
  constexpr std::string_view entry = "transcript/transcript.json";
  if (!has_entry(layout, std::string{entry})) {
    return;
  }

  const auto read_result =
      svp::package::read_package_entry(package_path, std::string{entry});
  if (!read_result.has_value()) {
    return;
  }

  nlohmann::json transcript;
  try {
    transcript = nlohmann::json::parse(read_result.value());
  } catch (const nlohmann::json::exception&) {
    return;
  }

  std::string diarization_status;
  if (transcript.contains("diarization") &&
      transcript["diarization"].contains("status") &&
      transcript["diarization"]["status"].is_string()) {
    diarization_status = transcript["diarization"]["status"].get<std::string>();
  }

  std::size_t speaker_count = 0;
  if (transcript.contains("speaker_count") &&
      transcript["speaker_count"].is_number()) {
    speaker_count = transcript["speaker_count"].get<std::size_t>();
  }

  if (diarization_status == "fallback_one_speaker") {
    add_finding(report, make_finding(registry, kCodeDiarizationFallback,
                                     "/transcript/transcript.json",
                                     "Speaker diarization did not run. Speaker count is "
                                     "unverified fallback, not real diarization. Install "
                                     "sherpa-onnx and rebuild."));
  } else if (diarization_status == "unavailable" || speaker_count == 0) {
    add_finding(report, make_finding(registry, kCodeDiarizationUnavailable,
                                     "/transcript/transcript.json",
                                     "Speaker diarization is unavailable. Package does not "
                                     "contain speaker data. Install sherpa-onnx and rebuild."));
  }
}

}  // namespace

void add_diarization_findings(ValidationReport& report,
                              const ValidationCodeRegistry& registry,
                              const std::filesystem::path& package_path,
                              const svp::package::PackageLayout& layout) {
  try {
    add_diarization_findings_impl(report, registry, package_path, layout);
  } catch (const std::exception&) {
  }
}

}  // namespace svp::validation
