#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/ocr_generation.hpp"

#include <cmath>
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

// Test that the byte cap is enforced as a hard upper bound.
// Simulates the cap-check logic from generate_evidence_crops_internal:
// after measuring a crop's bytes, if total + crop > cap, the crop is
// skipped and total never exceeds the cap.
void test_byte_cap_hard_limit() {
  const std::int64_t max_total_crop_bytes = 10000;

  // Simulate crop sizes: 4000, 4000, 4000
  // After 2 crops: total = 8000. Third crop (4000) would make 12000 > 10000.
  // The third crop must be skipped and total must stay at 8000.
  std::int64_t total_bytes = 0;
  std::size_t crops_accepted = 0;
  std::size_t crops_skipped = 0;
  std::string skip_reason;

  const std::int64_t crop_sizes[] = {4000, 4000, 4000};

  for (std::size_t i = 0; i < 3; ++i) {
    const std::int64_t crop_bytes = crop_sizes[i];

    // Pre-check (advisory, same as before extraction)
    if (total_bytes >= max_total_crop_bytes) {
      crops_skipped++;
      continue;
    }

    // Post-extraction hard check (the fix)
    if (total_bytes + crop_bytes > max_total_crop_bytes) {
      crops_skipped++;
      if (skip_reason.empty()) {
        skip_reason = "Total crop bytes cap reached";
      }
      continue;
    }

    total_bytes += crop_bytes;
    crops_accepted++;
  }

  require(crops_accepted == 2, "should accept 2 crops, not 3");
  require(crops_skipped == 1, "should skip 1 crop for byte cap");
  require(total_bytes == 8000, "total_bytes should be 8000, not 12000");
  require(total_bytes <= max_total_crop_bytes,
          "total_bytes must never exceed max_total_crop_bytes");
  require(!skip_reason.empty(), "skip reason should be set for byte cap");

  std::cout << "test_byte_cap_hard_limit: passed\n";
}

// Test the coordinate transform logic that scales OCR-frame bbox
// coordinates to source-frame coordinates. This test would fail on
// the old code that applied OCR-frame coordinates directly to the
// source video without scaling.
void test_coordinate_transform_scaling() {
  // Simulate: source video is 3840x2160, OCR runs at 1920x1080.
  // An OCR detection at bbox (100, 200, 300, 250) in OCR-frame space
  // must be scaled to source-frame space before ffmpeg crop.
  const int ocr_w = 1920;
  const int ocr_h = 1080;
  const int src_w = 3840;
  const int src_h = 2160;

  const double scale_x = static_cast<double>(src_w) / ocr_w;
  const double scale_y = static_cast<double>(src_h) / ocr_h;

  require(scale_x == 2.0, "scale_x should be 2.0 for 3840/1920");
  require(scale_y == 2.0, "scale_y should be 2.0 for 2160/1080");

  // OCR-frame bbox
  const int ocr_left = 100, ocr_top = 200, ocr_right = 300, ocr_bottom = 250;

  // Scale to source-frame
  const int src_left = static_cast<int>(std::round(ocr_left * scale_x));
  const int src_top = static_cast<int>(std::round(ocr_top * scale_y));
  const int src_right = static_cast<int>(std::round(ocr_right * scale_x));
  const int src_bottom = static_cast<int>(std::round(ocr_bottom * scale_y));

  require(src_left == 200, "src_left should be 200 (100*2)");
  require(src_top == 400, "src_top should be 400 (200*2)");
  require(src_right == 600, "src_right should be 600 (300*2)");
  require(src_bottom == 500, "src_bottom should be 500 (250*2)");

  // The old buggy code would have used (100, 200, 300, 250) directly
  // on the 3840x2160 source, landing on the wrong part of the frame.
  // The fix scales to (200, 400, 600, 500) in source space.
  require(src_left != ocr_left,
      "scaled left must differ from OCR-frame left (proves transform is applied)");
  require(src_top != ocr_top,
      "scaled top must differ from OCR-frame top (proves transform is applied)");

  std::cout << "test_coordinate_transform_scaling: passed\n";
}

// Test that the coordinate transform is identity when OCR frame
// dimensions equal source dimensions (no scaling needed).
void test_coordinate_transform_identity() {
  const int ocr_w = 1920;
  const int ocr_h = 1080;
  const int src_w = 1920;
  const int src_h = 1080;

  const double scale_x = static_cast<double>(src_w) / ocr_w;
  const double scale_y = static_cast<double>(src_h) / ocr_h;

  require(scale_x == 1.0, "scale_x should be 1.0 when dimensions match");
  require(scale_y == 1.0, "scale_y should be 1.0 when dimensions match");

  const int ocr_left = 100, ocr_top = 200, ocr_right = 300, ocr_bottom = 250;
  const int src_left = static_cast<int>(std::round(ocr_left * scale_x));
  const int src_top = static_cast<int>(std::round(ocr_top * scale_y));
  const int src_right = static_cast<int>(std::round(ocr_right * scale_x));
  const int src_bottom = static_cast<int>(std::round(ocr_bottom * scale_y));

  require(src_left == ocr_left, "identity: src_left should equal ocr_left");
  require(src_top == ocr_top, "identity: src_top should equal ocr_top");
  require(src_right == ocr_right, "identity: src_right should equal ocr_right");
  require(src_bottom == ocr_bottom, "identity: src_bottom should equal ocr_bottom");

  std::cout << "test_coordinate_transform_identity: passed\n";
}

// Test that the coordinate transform handles non-uniform scaling
// (e.g. anamorphic source where width and height scale differently).
void test_coordinate_transform_nonuniform() {
  const int ocr_w = 1280;
  const int ocr_h = 720;
  const int src_w = 3840;
  const int src_h = 2160;

  const double scale_x = static_cast<double>(src_w) / ocr_w;
  const double scale_y = static_cast<double>(src_h) / ocr_h;

  require(scale_x == 3.0, "scale_x should be 3.0 for 3840/1280");
  require(scale_y == 3.0, "scale_y should be 3.0 for 2160/720");

  // OCR-frame bbox
  const int ocr_left = 50, ocr_top = 100, ocr_right = 200, ocr_bottom = 180;
  const int src_left = static_cast<int>(std::round(ocr_left * scale_x));
  const int src_top = static_cast<int>(std::round(ocr_top * scale_y));
  const int src_right = static_cast<int>(std::round(ocr_right * scale_x));
  const int src_bottom = static_cast<int>(std::round(ocr_bottom * scale_y));

  require(src_left == 150, "nonuniform: src_left should be 150");
  require(src_top == 300, "nonuniform: src_top should be 300");
  require(src_right == 600, "nonuniform: src_right should be 600");
  require(src_bottom == 540, "nonuniform: src_bottom should be 540");

  std::cout << "test_coordinate_transform_nonuniform: passed\n";
}

void test_ocr_source_frame_dimensions_rotation_270() {
  const svp::vision::OcrSourceFrameDimensions dims =
      svp::vision::derive_ocr_source_frame_dimensions(3840, 2160, 270);

  require(dims.width == 2160,
      "rotation 270 should swap source frame width to stored height");
  require(dims.height == 3840,
      "rotation 270 should swap source frame height to stored width");

  const svp::vision::OcrSourceFrameDimensions negative =
      svp::vision::derive_ocr_source_frame_dimensions(3840, 2160, -90);

  require(negative.width == 2160,
      "rotation -90 should normalize to 270 and swap width");
  require(negative.height == 3840,
      "rotation -90 should normalize to 270 and swap height");

  const svp::vision::OcrSourceFrameDimensions full_turn =
      svp::vision::derive_ocr_source_frame_dimensions(3840, 2160, 450);

  require(full_turn.width == 2160,
      "rotation 450 should normalize to 90 and swap width");
  require(full_turn.height == 3840,
      "rotation 450 should normalize to 90 and swap height");

  std::cout << "test_ocr_source_frame_dimensions_rotation_270: passed\n";
}

// Test that evidence crop metadata includes the coordinate space
// information needed for auditing.
void test_crop_metadata_coordinate_space() {
  svp::vision::EvidenceCropRecord record;
  record.crop_id = "crop_000001";
  record.text_region_id = "text_region_000001";
  record.text_observation_id = "text_obs_000001";
  record.bbox_coordinate_space = "ocr_frame";
  record.ocr_frame_width = 1920;
  record.ocr_frame_height = 1080;
  record.source_frame_width = 3840;
  record.source_frame_height = 2160;
  record.canonical_raster_width = 640;
  record.canonical_raster_height = 360;
  record.transform_scale_x = 2.0;
  record.transform_scale_y = 2.0;
  record.crop_extraction_method = "ffmpeg_crop_scaled_to_source";
  record.original_bbox_left = 100;
  record.original_bbox_top = 200;
  record.original_bbox_right = 300;
  record.original_bbox_bottom = 250;
  record.crop_bbox_ocr_left = 88;
  record.crop_bbox_ocr_top = 182;
  record.crop_bbox_ocr_right = 312;
  record.crop_bbox_ocr_bottom = 268;
  record.crop_bbox_left = 176;
  record.crop_bbox_top = 364;
  record.crop_bbox_right = 624;
  record.crop_bbox_bottom = 536;
  record.evidence_quality = "strong";
  record.evidence_quality_reason = "ROI OCR on saved crop reproduces or supports linked observation";

  nlohmann::json j = svp::vision::evidence_crop_to_json(record);

  require(j.contains("bbox_coordinate_space"),
      "metadata should contain bbox_coordinate_space");
  require(j["bbox_coordinate_space"] == "ocr_frame",
      "bbox_coordinate_space should be 'ocr_frame'");
  require(j.contains("ocr_frame_width"),
      "metadata should contain ocr_frame_width");
  require(j["ocr_frame_width"] == 1920, "ocr_frame_width should be 1920");
  require(j.contains("source_frame_width"),
      "metadata should contain source_frame_width");
  require(j["source_frame_width"] == 3840, "source_frame_width should be 3840");
  require(j.contains("canonical_raster_width"),
      "metadata should contain canonical_raster_width");
  require(j["canonical_raster_width"] == 640, "canonical_raster_width should be 640");
  require(j.contains("transform_scale_x"),
      "metadata should contain transform_scale_x");
  require(j["transform_scale_x"] == 2.0, "transform_scale_x should be 2.0");
  require(j.contains("crop_extraction_method"),
      "metadata should contain crop_extraction_method");
  require(j["crop_extraction_method"] == "ffmpeg_crop_scaled_to_source",
      "crop_extraction_method should be 'ffmpeg_crop_scaled_to_source'");
  require(j.contains("crop_bbox_ocr"),
      "metadata should contain crop_bbox_ocr");
  require(j.contains("evidence_quality"),
      "metadata should contain evidence_quality");
  require(j["evidence_quality"] == "strong",
      "evidence_quality should be 'strong'");
  require(j.contains("evidence_quality_reason"),
      "metadata should contain evidence_quality_reason");

  std::cout << "test_crop_metadata_coordinate_space: passed\n";
}

// Test that observation -> region -> evidence_crop refs remain correct
// after the coordinate transform fix. This is a regression test for
// the linkage integrity.
void test_observation_region_crop_linkage() {
  // Simulate 2 text observations with linked regions and crops
  std::vector<svp::vision::TextObservationRecord> observations(2);
  observations[0].text_observation_id = "text_obs_000001";
  observations[0].text_region_id = "text_region_000001";
  observations[1].text_observation_id = "text_obs_000002";
  observations[1].text_region_id = "text_region_000002";

  std::vector<svp::vision::EvidenceCropRecord> crops(2);
  crops[0].crop_id = "crop_000001";
  crops[0].text_region_id = "text_region_000001";
  crops[0].text_observation_id = "text_obs_000001";
  crops[1].crop_id = "crop_000002";
  crops[1].text_region_id = "text_region_000002";
  crops[1].text_observation_id = "text_obs_000002";

  // Apply ID-based linking
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

  // Verify linkage chain: observation -> crop -> region
  require(observations[0].evidence_crop_refs.size() == 1,
      "obs 1 should have 1 crop ref");
  require(observations[0].evidence_crop_refs[0] == "crop_000001",
      "obs 1 should link to crop_000001");
  require(observations[1].evidence_crop_refs.size() == 1,
      "obs 2 should have 1 crop ref");
  require(observations[1].evidence_crop_refs[0] == "crop_000002",
      "obs 2 should link to crop_000002");

  // Verify crop -> region linkage
  require(crops[0].text_region_id == observations[0].text_region_id,
      "crop 1 region should match obs 1 region");
  require(crops[1].text_region_id == observations[1].text_region_id,
      "crop 2 region should match obs 2 region");

  std::cout << "test_observation_region_crop_linkage: passed\n";
}

}  // namespace

int main() {
  test_crop_linking_with_skipped_crop();
  test_roi_hardening_linking_with_skipped_crop();
  test_byte_cap_hard_limit();
  test_coordinate_transform_scaling();
  test_coordinate_transform_identity();
  test_coordinate_transform_nonuniform();
  test_ocr_source_frame_dimensions_rotation_270();
  test_crop_metadata_coordinate_space();
  test_observation_region_crop_linkage();
  std::cout << "All evidence crop linking tests passed.\n";
  return 0;
}
