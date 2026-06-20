#include "svp/vision/foundation_color_staging.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  const svp::vision::FoundationColorStagingArtifact artifact =
      svp::vision::build_foundation_color_staging_artifact();
  const nlohmann::json json =
      svp::vision::foundation_color_staging_artifact_to_json(artifact);

  require(artifact.records.records.size() == 6,
          "staging artifact should include deterministic frame, scene, and shot records");
  require(json.at("manifest").at("execution_state") ==
              "foundation_synthetic_sample_only",
          "staging manifest should identify the foundation-only state");
  require(json.at("manifest").at("input_kind") ==
              "synthetic_in_memory_color_frames",
          "staging manifest should identify synthetic in-memory input");
  require(json.at("manifest").at("real_media_frame_decoding_run") == false,
          "staging manifest should not claim real media frame decoding");
  require(json.at("manifest").at("package_writer_run") == false,
          "staging manifest should not claim package writing");
  require(json.at("manifest").at("valid_svp_package_written") == false,
          "staging manifest should not claim a valid SVP package");
  require(json.at("colors").at("color_observations").size() == 6,
          "staging JSON should expose color observation records");
  require(json.at("colors").at("color_summary").at("color_observation_count") == 6,
          "staging JSON should expose color summary");
  require(json.at("colors").at("color_absence").at("color_completed") == true,
          "staging JSON should expose completed color absence state");
  require(json.at("provenance").at("processors").at(0).at("id") ==
              "processor_color_quantizer_0001",
          "staging JSON should expose processor provenance");

  return 0;
}
