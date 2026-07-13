#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: svp-visual-entity-diagnostic "
                 "<media> <model-cache> <ffmpeg> <ffprobe> "
                 "[--include-embeddings]\n";
    return 2;
  }

  try {
    const std::filesystem::path media_path = argv[1];
    const std::filesystem::path model_cache = argv[2];
    const std::filesystem::path ffmpeg = argv[3];
    const std::filesystem::path ffprobe = argv[4];
    const bool include_embeddings = argc == 6 &&
        std::string(argv[5]) == "--include-embeddings";
    if (argc == 6 && !include_embeddings) {
      throw std::invalid_argument("unknown diagnostic option: " +
                                  std::string(argv[5]));
    }
    auto probe = svp::media::probe_media_with_ffprobe(media_path, ffprobe);
    auto plan = svp::media::build_media_ingest_plan(media_path, std::move(probe));
    auto result = svp::vision::run_visual_entity_pipeline(
        plan, ffmpeg, model_cache, {});

    nlohmann::json entities = nlohmann::json::array();
    for (const auto& entity : result.assembled.tracker_result.entities) {
      entities.push_back({
          {"id", entity.entity_id},
          {"entity_type", entity.entity_type},
          {"first_seen_us", entity.first_seen_us},
          {"last_seen_us", entity.last_seen_us},
          {"average_visibility", entity.average_visibility},
          {"average_screen_area", entity.average_screen_area}});
    }
    nlohmann::json tracks = nlohmann::json::array();
    for (const auto& track : result.assembled.tracker_result.tracks) {
      tracks.push_back({
          {"id", track.track_id},
          {"entity_id", track.entity_id},
          {"start_us", track.start_us},
          {"end_us", track.end_us},
          {"region_count", track.region_count},
          {"candidate_source", track.candidate_source}});
    }
    nlohmann::json regions = nlohmann::json::array();
    for (const auto& region : result.assembled.tracker_result.regions) {
      nlohmann::json diagnostic_region = {
          {"id", region.region_id},
          {"entity_id", region.entity_id},
          {"track_id", region.track_id},
          {"frame_id", region.frame_id},
          {"pts_us", region.timestamp_us},
          {"box_norm", {region.box_norm[0], region.box_norm[1],
                         region.box_norm[2], region.box_norm[3]}},
          {"screen_area_ratio", region.screen_area_ratio},
          {"candidate_source", region.candidate_source},
          {"internal_detector_category_index",
           region.detector_category_index}};
      if (!region.embedding.empty()) {
        diagnostic_region["embedding_dimension"] = region.embedding.size();
        if (include_embeddings) {
          diagnostic_region["embedding"] = region.embedding;
        }
      }
      regions.push_back(std::move(diagnostic_region));
    }
    nlohmann::json output = {
        {"windows_planned", result.windows_planned},
        {"windows_processed", result.windows_processed},
        {"frames_attempted", result.frames_attempted},
        {"frames_decoded", result.frames_decoded},
        {"frames_missed", result.frames_missed},
        {"entity_count", result.assembled.tracker_result.entities.size()},
        {"track_count", result.assembled.tracker_result.tracks.size()},
        {"region_count", result.assembled.tracker_result.regions.size()},
        {"mask_count", result.assembled.masks.size()},
        {"blocker", result.blocker},
        {"entities", std::move(entities)},
        {"tracks", std::move(tracks)},
        {"regions", std::move(regions)}};
    output["cut_evidence"] = nlohmann::json::array();
    for (const auto& evidence : result.cut_evidence) {
      output["cut_evidence"].push_back({
          {"timestamp_us", evidence.timestamp_us},
          {"difference", evidence.difference},
          {"immediate_following_difference",
           evidence.immediate_following_difference},
          {"minimum_lookahead_difference",
           evidence.minimum_lookahead_difference},
          {"is_sustained_transition", evidence.is_sustained_transition},
          {"is_cut", evidence.is_cut}});
    }
    std::cout << output.dump(2) << '\n';
    return result.blocker.empty() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
