#include "svp/package/timeline_writer.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/foundation_color_staging.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cmath>
#include <nlohmann/json.hpp>

namespace svp::package {
namespace {

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
  std::vector<nlohmann::json> records;
  if (!std::filesystem::exists(path)) {
    return records;
  }
  std::ifstream input(path);
  if (!input) {
    return records;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    try {
      records.push_back(nlohmann::json::parse(line));
    } catch (...) {
    }
  }
  return records;
}

void write_jsonl(const std::filesystem::path& path,
                 const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

std::string string_value(const nlohmann::json& record, const char* key) {
  const auto iterator = record.find(key);
  if (iterator == record.end() || !iterator->is_string()) {
    return {};
  }
  return iterator->get<std::string>();
}

void append_processor_records(
    const std::filesystem::path& processors_path,
    const std::vector<nlohmann::json>& new_processors) {
  std::map<std::string, nlohmann::json> processors_by_id;
  for (const nlohmann::json& processor : read_jsonl(processors_path)) {
    const std::string id = string_value(processor, "id");
    if (!id.empty()) {
      processors_by_id[id] = processor;
    }
  }
  for (const nlohmann::json& processor : new_processors) {
    const std::string id = string_value(processor, "id");
    if (!id.empty()) {
      processors_by_id[id] = processor;
    }
  }
  std::vector<nlohmann::json> all_processors;
  all_processors.reserve(processors_by_id.size());
  for (const auto& [id, processor] : processors_by_id) {
    all_processors.push_back(processor);
  }
  write_jsonl(processors_path, all_processors);
}

} // namespace

TimelineWriteSummary write_timeline_artifacts(
    const std::filesystem::path& staging_dir,
    const svp::media::MediaIngestPlan& plan,
    const svp::vision::FoundationColorStagingArtifact& color_artifact) {

  TimelineWriteSummary summary;

  const auto& sampling = color_artifact.sampling_input;

  // Determine media duration (with fallbacks if invalid or synthetic)
  std::int64_t duration_us = svp::vision::compute_media_duration_us(plan);
  if (duration_us <= 0) {
    if (!sampling.frames.empty()) {
      duration_us = sampling.frames.back().timestamp_us;
      if (duration_us == 0) {
        duration_us = 1000000;
      }
    } else {
      duration_us = 1000000;
    }
  }

  // 1. Generate non-overlapping, contiguous shot intervals
  std::vector<nlohmann::json> shot_records;
  shot_records.reserve(sampling.shots.size());

  const std::size_t num_shots = sampling.shots.size();

  // Create a map from frame_id to timestamp_us for quick lookup
  std::map<std::string, std::int64_t> frame_ts_map;
  for (const auto& f : sampling.frames) {
    frame_ts_map[f.frame_id] = f.timestamp_us;
  }

  // Helper to get shot frame timestamps
  auto get_shot_timestamps = [&](const auto& shot) -> std::pair<std::int64_t, std::int64_t> {
    std::int64_t min_ts = -1;
    std::int64_t max_ts = -1;
    for (const auto& fid : shot.frame_ids) {
      auto it = frame_ts_map.find(fid);
      if (it != frame_ts_map.end()) {
        if (min_ts == -1 || it->second < min_ts) min_ts = it->second;
        if (max_ts == -1 || it->second > max_ts) max_ts = it->second;
      }
    }
    if (min_ts == -1) {
      return {shot.start_us, shot.end_us};
    }
    return {min_ts, max_ts};
  };

  std::vector<std::int64_t> B(num_shots + 1, 0);
  B[0] = 0;
  B[num_shots] = duration_us;

  for (std::size_t i = 1; i < num_shots; ++i) {
    auto prev_range = get_shot_timestamps(sampling.shots[i - 1]);
    auto curr_range = get_shot_timestamps(sampling.shots[i]);
    B[i] = (prev_range.second + curr_range.first) / 2;
  }

  // Enforce monotonicity
  std::int64_t delta = (duration_us >= static_cast<std::int64_t>(num_shots)) ? 1 : 0;
  for (std::size_t i = 1; i <= num_shots; ++i) {
    if (B[i] < B[i - 1] + delta) {
      B[i] = B[i - 1] + delta;
    }
  }
  if (B[num_shots] > duration_us) {
    B[num_shots] = duration_us;
    for (std::size_t i = num_shots; i > 0; --i) {
      if (B[i - 1] > B[i] - delta) {
        B[i - 1] = B[i] - delta;
      }
    }
  }

  for (std::size_t i = 0; i < num_shots; ++i) {
    const auto& shot = sampling.shots[i];
    std::string start_frame = shot.frame_ids.empty() ? "" : shot.frame_ids.front();
    std::string end_frame = shot.frame_ids.empty() ? "" : shot.frame_ids.back();

    std::int64_t start_us = B[i];
    std::int64_t end_us = B[i + 1];

    std::string cut_in = "hard_cut";
    std::string cut_out = "hard_cut";

    if (i == 0) {
      cut_in = "source_start";
    }
    if (i + 1 == num_shots) {
      cut_out = "source_end";
    }

    nlohmann::json shot_rec = {
        {"id", shot.target_id},
        {"start_us", start_us},
        {"end_us", end_us},
        {"start_frame_id", start_frame},
        {"end_frame_id", end_frame},
        {"cut_type_in", cut_in},
        {"cut_type_out", cut_out},
        {"visual_change_score", 0.0},
        {"dominant_motion", "static"},
        {"black_frame_ratio", 0.0},
        {"processor_id", "processor_timeline_generator_0001"},
        {"method", "honest conservative color-segmentation-derived shot grouping"}
    };
    shot_records.push_back(std::move(shot_rec));
  }

  // 2. Write shots.jsonl
  const std::filesystem::path timeline_dir = staging_dir / "timeline";
  write_jsonl(timeline_dir / "shots.jsonl", shot_records);
  summary.shots_written = true;
  summary.shot_count = shot_records.size();

  // 3. Write scenes.jsonl (intervals coherently containing shot intervals)
  std::vector<nlohmann::json> scene_records;
  scene_records.reserve(sampling.scenes.size());

  for (std::size_t i = 0; i < sampling.scenes.size(); ++i) {
    const auto& scene = sampling.scenes[i];

    std::vector<std::string> shot_ids;
    std::vector<std::size_t> shot_indices;
    for (std::size_t k = 0; k < num_shots; ++k) {
      const auto& shot_item = sampling.shots[k];
      bool overlaps = false;
      for (const auto& fid : shot_item.frame_ids) {
        if (std::find(scene.frame_ids.begin(), scene.frame_ids.end(), fid) != scene.frame_ids.end()) {
          overlaps = true;
          break;
        }
      }
      if (overlaps) {
        shot_ids.push_back(shot_item.target_id);
        shot_indices.push_back(k);
      }
    }

    std::int64_t scene_start = scene.start_us;
    std::int64_t scene_end = scene.end_us;

    if (!shot_indices.empty()) {
      std::size_t min_idx = *std::min_element(shot_indices.begin(), shot_indices.end());
      std::size_t max_idx = *std::max_element(shot_indices.begin(), shot_indices.end());
      scene_start = shot_records[min_idx]["start_us"].get<std::int64_t>();
      scene_end = shot_records[max_idx]["end_us"].get<std::int64_t>();
    }

    nlohmann::json scene_rec = {
        {"id", scene.target_id},
        {"start_us", scene_start},
        {"end_us", scene_end},
        {"shot_ids", shot_ids},
        {"dominant_color_summary_ref", "colors/color_summary.json"},
        {"audio_continuity_score", 1.0},
        {"visual_continuity_score", 1.0},
        {"transcript_topic_shift_score", 0.0},
        {"processor_id", "processor_timeline_generator_0001"},
        {"method", "honest conservative color-segmentation-derived scene grouping"}
    };

    scene_records.push_back(std::move(scene_rec));
  }

  write_jsonl(timeline_dir / "scenes.jsonl", scene_records);
  summary.scenes_written = true;
  summary.scene_count = scene_records.size();

  // 4. Write frames.jsonl
  std::vector<nlohmann::json> frame_records;
  frame_records.reserve(sampling.frames.size());

  const std::string stream_id = plan.primary_video_stream.id.empty() ? "vstream_0001" : plan.primary_video_stream.id;

  const bool has_real_video = plan.primary_video_stream.width > 0;
  const std::int32_t src_w = has_real_video ? plan.primary_video_stream.width : 640;
  const std::int32_t src_h = has_real_video ? plan.primary_video_stream.height : 360;
  const std::int32_t rot = has_real_video ? plan.primary_video_stream.rotation_degrees : 0;
  const std::int32_t analysis_w = has_real_video ? plan.canonical_raster.width : 640;
  const std::int32_t analysis_h = has_real_video ? plan.canonical_raster.height : 360;

  std::int32_t display_w = src_w;
  std::int32_t display_h = src_h;
  std::string par = "1:1";

  if (has_real_video) {
    display_w = plan.canonical_raster.display.oriented_width;
    display_h = plan.canonical_raster.display.oriented_height;
    par = std::to_string(plan.primary_video_stream.pixel_aspect_ratio.numerator) + ":" +
          std::to_string(plan.primary_video_stream.pixel_aspect_ratio.denominator);
  }

  for (std::size_t i = 0; i < sampling.frames.size(); ++i) {
    const auto& frame = sampling.frames[i];

    std::string shot_id;
    for (const auto& shot : sampling.shots) {
      if (std::find(shot.frame_ids.begin(), shot.frame_ids.end(), frame.frame_id) != shot.frame_ids.end()) {
        shot_id = shot.target_id;
        break;
      }
    }

    std::string scene_id;
    for (const auto& scene : sampling.scenes) {
      if (std::find(scene.frame_ids.begin(), scene.frame_ids.end(), frame.frame_id) != scene.frame_ids.end()) {
        scene_id = scene.target_id;
        break;
      }
    }

    std::ostringstream pts_sec_oss;
    pts_sec_oss << std::fixed << std::setprecision(6) << (static_cast<double>(frame.timestamp_us) / 1000000.0);

    nlohmann::json frame_rec = {
        {"id", frame.frame_id},
        {"frame_index", i},
        {"source_stream_id", stream_id},
        {"pts_us", frame.timestamp_us},
        {"pts_sec", pts_sec_oss.str()},
        {"source_width", src_w},
        {"source_height", src_h},
        {"display_width", display_w},
        {"display_height", display_h},
        {"pixel_aspect_ratio", par},
        {"rotation_degrees_applied", rot},
        {"analysis_width", analysis_w},
        {"analysis_height", analysis_h}
    };
    if (!shot_id.empty()) {
      frame_rec["shot_id"] = shot_id;
    }
    if (!scene_id.empty()) {
      frame_rec["scene_id"] = scene_id;
    }

    frame_records.push_back(std::move(frame_rec));
  }

  write_jsonl(timeline_dir / "frames.jsonl", frame_records);
  summary.frames_written = true;
  summary.frame_count = frame_records.size();

  // 5. Append processor provenance with honest limitations
  nlohmann::json proc = {
      {"id", "processor_timeline_generator_0001"},
      {"name", "svp timeline generator"},
      {"version", "svp-timeline-generator-v1"},
      {"input_refs", {"media/media_000001"}},
      {"output_refs", {
          "timeline/frames.jsonl",
          "timeline/shots.jsonl",
          "timeline/scenes.jsonl"
      }},
      {"parameters", {
          {"method", "color_segmentation_derived"},
          {"limitations", "honest frame sampling and color-based grouping rather than cinematic shot detection"}
      }}
  };

  append_processor_records(staging_dir / "provenance" / "processors.jsonl", {proc});

  return summary;
}

} // namespace svp::package
