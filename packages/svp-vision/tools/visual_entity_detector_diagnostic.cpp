#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/vision/visual_entity_detector.hpp"
#include "svp/vision/visual_entity_frame_decoder.hpp"
#include "svp/vision/visual_entity_sampling.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: svp-visual-entity-detector-diagnostic "
                 "<media> <timestamp-us> <confidence> <model-cache> "
                 "<ffmpeg> <ffprobe>\n";
    return 2;
  }
  try {
    const std::filesystem::path media_path = argv[1];
    const auto timestamp_us = std::stoll(argv[2]);
    svp::vision::VisualEntityDetectorOptions detector_options;
    detector_options.confidence_threshold = std::stod(argv[3]);
    auto probe = svp::media::probe_media_with_ffprobe(media_path, argv[6]);
    auto plan = svp::media::build_media_ingest_plan(media_path, std::move(probe));
    const auto sampling_plan = svp::vision::make_visual_entity_sampling_plan(
        svp::vision::compute_media_duration_us(plan));
    const auto window = std::find_if(
        sampling_plan.begin(), sampling_plan.end(), [&](const auto& candidate) {
          return candidate.start_us <= timestamp_us &&
              candidate.end_us >= timestamp_us;
        });
    if (window == sampling_plan.end()) {
      throw std::runtime_error("requested timestamp is outside sampling plan");
    }
    auto decoded = svp::vision::decode_visual_entity_window(
        plan, argv[5], plan.canonical_raster.width,
        plan.canonical_raster.height, window->timestamps_us);
    if (!decoded.decoding_succeeded || decoded.frames.empty()) {
      throw std::runtime_error("failed to decode requested diagnostic frame");
    }
    auto detector = svp::vision::load_visual_entity_detector(
        argv[4], detector_options);
    if (!detector.session) throw std::runtime_error(detector.blocker);
    nlohmann::json detections = nlohmann::json::array();
    const auto frame = std::min_element(
        decoded.frames.begin(), decoded.frames.end(),
        [&](const auto& left, const auto& right) {
          return std::abs(left.timestamp_us - timestamp_us) <
              std::abs(right.timestamp_us - timestamp_us);
        });
    for (const auto& detection :
         svp::vision::detect_visual_entities(detector, *frame)) {
      detections.push_back({
          {"box_px", {detection.box_px[0], detection.box_px[1],
                       detection.box_px[2], detection.box_px[3]}},
          {"confidence", detection.confidence},
          {"internal_category_index", detection.category_index}});
    }
    std::cout << nlohmann::json({
        {"timestamp_us", timestamp_us},
        {"decoded_timestamp_us", frame->timestamp_us},
        {"confidence_threshold", detector_options.confidence_threshold},
        {"detections", std::move(detections)}}).dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
