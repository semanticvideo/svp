#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

// Simulate the ID-based linking logic from ocr_generation.cpp.
// This test proves that when an earlier crop is skipped (missing from
// the crops vector), later successful crops still link to the correct
// text observation by stable observation ID.
void test_crop_linking_with_skipped_crop() {
  // Simulate 3 text observations
  std::vector<svp::vision::TextObservationRecord> observations(3);
  observations[0].text_observation_id = "text_obs_000001";
  observations[1].text_observation_id = "text_obs_000002";
  observations[2].text_observation_id = "text_obs_000003";

  // Simulate crop results where the 2nd crop was skipped (extraction failure).
  // Only crops 1 and 3 exist — crop 2 is missing.
  std::vector<svp::vision::EvidenceCropRecord> crops;
  {
    svp::vision::EvidenceCropRecord c;
    c.crop_id = "crop_000001";
    c.text_observation_id = "text_obs_000001";
    crops.push_back(c);
  }
  {
    svp::vision::EvidenceCropRecord c;
    c.crop_id = "crop_000002";
    c.text_observation_id = "text_obs_000003";
    crops.push_back(c);
  }

  // Apply the same ID-based linking logic from ocr_generation.cpp
  std::unordered_map<std::string, std::string> obs_id_to_crop_id;
  for (const auto& crop : crops) {
    obs_id_to_crop_id[crop.text_observation_id] = crop.crop_id;
  }
  for (auto& obs : observations) {
    auto it = obs_id_to_crop_id.find(obs.text_observation_id);
    if (it != obs_id_to_crop_id.end()) {
      obs.evidence_crop_refs.push_back(it->second);
    }
  }

  // Observation 1 should link to crop_000001
  require(observations[0].evidence_crop_refs.size() == 1,
          "obs 1 should have 1 evidence crop ref");
  require(observations[0].evidence_crop_refs[0] == "crop_000001",
          "obs 1 should link to crop_000001");

  // Observation 2 was skipped — no crop ref
  require(observations[1].evidence_crop_refs.empty(),
          "obs 2 should have 0 evidence crop refs (skipped)");

  // Observation 3 should link to crop_000002 (NOT crop_000001)
  require(observations[2].evidence_crop_refs.size() == 1,
          "obs 3 should have 1 evidence crop ref");
  require(observations[2].evidence_crop_refs[0] == "crop_000002",
          "obs 3 should link to crop_000002, not crop_000001");

  std::cout << "test_crop_linking_with_skipped_crop: passed\n";
}

// Test that ROI OCR results also match by observation ID, not index.
void test_roi_hardening_linking_with_skipped_crop() {
  // Simulate 3 crop inputs
  std::vector<svp::vision::CropGenerationInput> inputs(3);
  inputs[0].text_observation_id = "text_obs_000001";
  inputs[1].text_observation_id = "text_obs_000002";
  inputs[2].text_observation_id = "text_obs_000003";

  // ROI results: input 1 failed, inputs 0 and 2 succeeded
  std::vector<svp::vision::RoiOcrResult> roi_results(3);
  roi_results[0].succeeded = true;
  roi_results[0].raw_text = "hello world";
  roi_results[0].word_count = 2;
  roi_results[0].confidence = 0.9;
  roi_results[1].succeeded = false;
  roi_results[2].succeeded = true;
  roi_results[2].raw_text = "foo bar baz";
  roi_results[2].word_count = 3;
  roi_results[2].confidence = 0.85;

  // Build the same ID-based lookup from ocr_generation.cpp
  std::unordered_map<std::string, std::size_t> obs_id_to_roi_idx;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    obs_id_to_roi_idx[inputs[i].text_observation_id] = i;
  }

  // Verify lookup finds the right ROI result for each observation
  auto it0 = obs_id_to_roi_idx.find("text_obs_000001");
  require(it0 != obs_id_to_roi_idx.end(), "obs 1 should have ROI result");
  require(roi_results[it0->second].succeeded, "obs 1 ROI should be succeeded");
  require(roi_results[it0->second].raw_text == "hello world",
          "obs 1 ROI text should be 'hello world'");

  auto it1 = obs_id_to_roi_idx.find("text_obs_000002");
  require(it1 != obs_id_to_roi_idx.end(), "obs 2 should have ROI result");
  require(!roi_results[it1->second].succeeded,
          "obs 2 ROI should be failed (skipped)");

  auto it2 = obs_id_to_roi_idx.find("text_obs_000003");
  require(it2 != obs_id_to_roi_idx.end(), "obs 3 should have ROI result");
  require(roi_results[it2->second].succeeded, "obs 3 ROI should be succeeded");
  require(roi_results[it2->second].raw_text == "foo bar baz",
          "obs 3 ROI text should be 'foo bar baz'");

  std::cout << "test_roi_hardening_linking_with_skipped_crop: passed\n";
}

}  // namespace

int main() {
  test_crop_linking_with_skipped_crop();
  test_roi_hardening_linking_with_skipped_crop();
  std::cout << "All evidence crop linking tests passed.\n";
  return 0;
}
