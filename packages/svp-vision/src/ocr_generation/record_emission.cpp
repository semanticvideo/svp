#include "ocr_generation_internal.hpp"

#include <cmath>
#include <optional>

namespace svp::vision::ocr_generation_internal {

void emit_reconciled_records(
    const OcrGenerationOptions& options,
    const std::vector<ReconciledObservation>& reconciled,
    OcrGenerationResult& result) {
  int region_counter = 0;
  int obs_counter = 0;
  int numeric_counter = 0;

  for (const auto& robs : reconciled) {
    if (options.target_max_observations > 0 &&
        static_cast<std::size_t>(obs_counter) >= options.target_max_observations) {
      result.observation_count_capped = true;
      break;
    }

    ++region_counter;
    ++obs_counter;

    const std::string region_id = pad_id("text_region_", region_counter);
    const std::string obs_id = pad_id("text_obs_", obs_counter);

    const int frame_w = robs.frame_width;
    const int frame_h = robs.frame_height;
    const int raster_w = options.canonical_raster_width > 0
        ? options.canonical_raster_width
        : frame_w;
    const int raster_h = options.canonical_raster_height > 0
        ? options.canonical_raster_height
        : frame_h;

    TextRegionRecord region;
    region.text_region_id = region_id;
    region.observation_type = "text_detection";
    region.start_us = robs.start_us;
    region.end_us = robs.end_us;
    region.frame_start = static_cast<std::int64_t>(robs.frame_start);
    region.frame_end = static_cast<std::int64_t>(robs.frame_end);
    region.bbox_norm = {
        static_cast<double>(robs.bbox_left) / frame_w,
        static_cast<double>(robs.bbox_top) / frame_h,
        static_cast<double>(robs.bbox_right) / frame_w,
        static_cast<double>(robs.bbox_bottom) / frame_h,
    };
    region.bbox_px = {
        static_cast<int>(std::round(static_cast<double>(robs.bbox_left) * raster_w / frame_w)),
        static_cast<int>(std::round(static_cast<double>(robs.bbox_top) * raster_h / frame_h)),
        static_cast<int>(std::round(static_cast<double>(robs.bbox_right) * raster_w / frame_w)),
        static_cast<int>(std::round(static_cast<double>(robs.bbox_bottom) * raster_h / frame_h)),
    };
    region.confidence = robs.confidence;
    region.provenance_id = "processor_ocr_detector_0001";
    result.text_regions.push_back(region);

    TextObservationRecord obs;
    obs.text_observation_id = obs_id;
    obs.text_region_id = region_id;
    obs.observation_type = "text_recognition";
    obs.raw_text = robs.raw_text;
    obs.normalized_text = robs.normalized_text;
    obs.language = nlohmann::json{
        {"primary", options.language}, {"script", "Latn"},
        {"mode", "multi"}, {"confidence", robs.confidence}};
    obs.confidence = robs.confidence;
    obs.layout_class = "scene_text";
    obs.source_frame_ids = robs.source_frame_ids;
    obs.provenance_id = "processor_ocr_recognizer_0001";
    result.text_observations.push_back(obs);

    auto numbers = parse_numeric_values(robs.raw_text, robs.confidence);
    for (const auto& num : numbers) {
      ++numeric_counter;
      NumericValueRecord nv;
      nv.numeric_value_id = pad_id("numeric_value_", numeric_counter);
      nv.text_observation_id = obs_id;
      nv.text_region_id = region_id;
      nv.raw_text = num.raw_text;
      nv.normalized_text = num.normalized_text;
      nv.number_kind = num.number_kind;
      nv.numeric_value = num.numeric_value;
      nv.unit = num.unit.empty()
          ? std::optional<std::string>{}
          : std::optional<std::string>{num.unit};
      nv.confidence = num.confidence;
      nv.parse_rule = "svp-number-parser-v1";
      nv.provenance_id = "processor_numeric_parser_0001";
      result.numeric_values.push_back(nv);
    }
  }

  result.ocr_recognition_run = true;
  result.text_region_count = static_cast<std::int64_t>(result.text_regions.size());
  result.text_observation_count =
      static_cast<std::int64_t>(result.text_observations.size());
  result.numeric_value_count = static_cast<std::int64_t>(result.numeric_values.size());
}

void refresh_numeric_values_from_observations(OcrGenerationResult& result) {
  result.numeric_values.clear();

  int numeric_counter = 0;
  for (const auto& obs : result.text_observations) {
    auto numbers = parse_numeric_values(obs.raw_text, obs.confidence);
    for (const auto& num : numbers) {
      ++numeric_counter;
      NumericValueRecord nv;
      nv.numeric_value_id = pad_id("numeric_value_", numeric_counter);
      nv.text_observation_id = obs.text_observation_id;
      nv.text_region_id = obs.text_region_id;
      nv.raw_text = num.raw_text;
      nv.normalized_text = num.normalized_text;
      nv.number_kind = num.number_kind;
      nv.numeric_value = num.numeric_value;
      nv.unit = num.unit.empty()
          ? std::optional<std::string>{}
          : std::optional<std::string>{num.unit};
      nv.confidence = num.confidence;
      nv.parse_rule = "svp-number-parser-v1";
      nv.provenance_id = "processor_numeric_parser_0001";
      result.numeric_values.push_back(nv);
    }
  }

  result.numeric_value_count = static_cast<std::int64_t>(result.numeric_values.size());
}

}  // namespace svp::vision::ocr_generation_internal
