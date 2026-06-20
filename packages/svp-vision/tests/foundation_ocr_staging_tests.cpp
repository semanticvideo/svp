#include "svp/vision/foundation_ocr_staging.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

void test_synthetic_artifact() {
  const svp::vision::FoundationOcrStagingArtifact artifact =
      svp::vision::build_foundation_ocr_staging_synthetic_artifact();
  const nlohmann::json json =
      svp::vision::foundation_ocr_staging_artifact_to_json(artifact);

  require(artifact.text_regions.size() == 1,
          "synthetic artifact should include 1 text region");
  require(artifact.text_observations.size() == 1,
          "synthetic artifact should include 1 text observation");
  require(artifact.numeric_values.size() == 1,
          "synthetic artifact should include 1 numeric value");

  const auto& manifest = json.at("manifest");
  require(manifest.at("execution_state") == "foundation_synthetic_sample_only",
          "synthetic manifest execution state should match");
  require(manifest.at("input_kind") == "synthetic_in_memory_color_frames",
          "synthetic manifest input kind should match");
  require(manifest.at("ocr_detection_run") == true,
          "synthetic manifest ocr_detection_run should be true");
  require(manifest.at("ocr_recognition_run") == true,
          "synthetic manifest ocr_recognition_run should be true");
  require(manifest.at("valid_svp_package_written") == false,
          "synthetic manifest valid_svp_package_written should be false");

  const auto& text = json.at("text");
  require(text.at("text_regions").size() == 1, "serialized regions count mismatch");
  require(text.at("text_observations").size() == 1, "serialized observations count mismatch");
  require(text.at("numeric_values").size() == 1, "serialized numeric values count mismatch");

  const auto& absence = text.at("text_absence");
  require(absence.at("ocr_completed") == true, "ocr_completed should be true");
  require(absence.at("reason") == "ocr_completed", "absence reason should match");
  require(absence.at("text_region_count") == 1, "absence region count mismatch");

  require(json.at("provenance").at("processors").size() == 4,
          "synthetic provenance should expose 4 processors");
}

void test_real_artifact_runtime_unavailable() {
  svp::media::MediaIngestPlan plan;
  plan.source_path = "dummy_video.mp4";

  const svp::vision::FoundationOcrStagingArtifact artifact =
      svp::vision::build_real_ocr_staging_artifact(plan, false);
  const nlohmann::json json =
      svp::vision::foundation_ocr_staging_artifact_to_json(artifact);

  require(artifact.text_regions.empty(), "real artifact should have no regions when not run");
  require(artifact.text_observations.empty(),
          "real artifact should have no observations when not run");
  require(artifact.numeric_values.empty(),
          "real artifact should have no numeric values when not run");

  const auto& manifest = json.at("manifest");
  require(manifest.at("execution_state") == "real_ocr_model_runtime_unavailable",
          "real manifest execution state should match");
  require(manifest.at("model_runtime_available") == false,
          "real manifest model_runtime_available should be false");
  require(manifest.at("ocr_detection_run") == false,
          "real manifest ocr_detection_run should be false");
  require(manifest.at("ocr_recognition_run") == false,
          "real manifest ocr_recognition_run should be false");
  require(manifest.at("text_regions_written") == true,
          "real manifest should report writing empty files");

  const auto& text = json.at("text");
  require(text.at("text_regions").empty(), "regions should be empty");
  require(text.at("text_observations").empty(), "observations should be empty");
  require(text.at("numeric_values").empty(), "numeric values should be empty");

  const auto& absence = text.at("text_absence");
  require(absence.at("ocr_completed") == false, "ocr_completed should be false");
  require(absence.at("reason") == "processor_failed", "absence reason should be processor_failed");
  require(absence.at("text_region_count") == 0, "absence region count should be 0");
}

void test_sanitize_utf8() {
  // 1. Valid ASCII
  require(svp::vision::sanitize_utf8("Hello, World!") == "Hello, World!",
          "valid ASCII should remain unchanged");

  // 2. Valid UTF-8 (Chinese characters: 世界)
  require(svp::vision::sanitize_utf8("Hello, \xE4\xB8\x96\xE7\x95\x8C!") == "Hello, \xE4\xB8\x96\xE7\x95\x8C!",
          "valid UTF-8 should remain unchanged");

  // 3. Invalid raw byte (0x89)
  require(svp::vision::sanitize_utf8("Hello \x89 World") == "Hello \xEF\xBF\xBD World",
          "invalid raw byte should be replaced with U+FFFD");

  // 4. Overlong 2-byte sequence (\xC0\xAF)
  // \xC0 starts a 2-byte sequence but since c < 0xC2 it is rejected.
  // The first byte \xC0 is replaced by \xEF\xBF\xBD.
  // The second byte \xAF is a continuation byte out of context, so it is also replaced.
  require(svp::vision::sanitize_utf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD",
          "overlong 2-byte sequence should be replaced");

  // 5. Surrogate halves (\xED\xA0\x80)
  // Starts with \xED, second byte is >= 0xA0 (0xA0). Replaced.
  // The next bytes \xA0 and \x80 are continuation bytes out of context, also replaced.
  require(svp::vision::sanitize_utf8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",
          "surrogate halves should be replaced");
}

}  // namespace

int main() {
  test_synthetic_artifact();
  test_real_artifact_runtime_unavailable();
  test_sanitize_utf8();
  std::cout << "All OCR staging foundation tests passed.\n";
  return 0;
}
