#include "svp/package/entity_writer.hpp"

#include "svp/vision/mask_writer.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace svp::package {
namespace {

constexpr const char* kEntityProcessorId = "processor_entity_writer_0001";

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

std::int64_t int_value_or_zero(const nlohmann::json& record, const char* key) {
  const auto iterator = record.find(key);
  if (iterator == record.end() || !iterator->is_number_integer()) {
    return 0;
  }
  return iterator->get<std::int64_t>();
}

double confidence_value_or_one(const nlohmann::json& record) {
  const auto iterator = record.find("confidence");
  if (iterator == record.end() || !iterator->is_number()) {
    return 1.0;
  }
  return iterator->get<double>();
}

struct TextRegionInfo {
  std::string text_region_id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
  double confidence = 1.0;
  std::string shot_id;
  std::string scene_id;
  std::string frame_start;
  std::string frame_end;
  std::vector<double> bbox_norm;
};

struct EntityGroup {
  std::string normalized_text;
  std::vector<TextRegionInfo> regions;
};

struct FrameIdLookup {
  std::map<std::int64_t, std::string> index_to_id;
  std::set<std::string> known_ids;
};

FrameIdLookup build_frame_id_lookup(const std::filesystem::path& staging_dir) {
  FrameIdLookup result;
  const auto frames = read_jsonl(staging_dir / "timeline" / "frames.jsonl");
  for (const auto& frame : frames) {
    const std::string id = string_value(frame, "id");
    if (!id.empty()) {
      result.known_ids.insert(id);
    }
    const auto idx_it = frame.find("frame_index");
    if (!id.empty() && idx_it != frame.end() && idx_it->is_number_integer()) {
      result.index_to_id[idx_it->get<std::int64_t>()] = id;
    }
  }
  return result;
}

std::string resolve_frame_ref(const nlohmann::json& record,
                              const char* key,
                              const FrameIdLookup& frame_lookup) {
  const auto it = record.find(key);
  if (it == record.end() || it->is_null()) {
    return {};
  }
  if (it->is_string()) {
    const std::string id = it->get<std::string>();
    if (frame_lookup.known_ids.count(id) > 0) {
      return id;
    }
    return {};
  }
  if (it->is_number_integer()) {
    const auto map_it = frame_lookup.index_to_id.find(it->get<std::int64_t>());
    if (map_it != frame_lookup.index_to_id.end()) {
      return map_it->second;
    }
  }
  return {};
}

std::map<std::string, std::string> build_region_to_text_map(
    const std::filesystem::path& staging_dir) {
  std::map<std::string, std::string> result;
  const auto observations = read_jsonl(staging_dir / "text" / "text_observations.jsonl");
  for (const auto& obs : observations) {
    const std::string region_id = string_value(obs, "text_region_id");
    const std::string normalized = string_value(obs, "normalized_text");
    if (!region_id.empty() && !normalized.empty()) {
      result[region_id] = normalized;
    }
  }
  return result;
}

std::vector<EntityGroup> group_text_regions(
    const std::filesystem::path& staging_dir,
    std::size_t& skipped_missing_evidence) {
  const auto text_regions = read_jsonl(staging_dir / "text" / "text_regions.jsonl");
  const auto region_to_text = build_region_to_text_map(staging_dir);
  const auto frame_lookup = build_frame_id_lookup(staging_dir);

  std::map<std::string, EntityGroup> groups_by_text;

  for (const auto& region : text_regions) {
    const std::string region_id = string_value(region, "text_region_id");
    if (region_id.empty()) {
      ++skipped_missing_evidence;
      continue;
    }

    const auto text_it = region_to_text.find(region_id);
    if (text_it == region_to_text.end()) {
      ++skipped_missing_evidence;
      continue;
    }

    TextRegionInfo info;
    info.text_region_id = region_id;
    info.start_us = int_value_or_zero(region, "start_us");
    info.end_us = int_value_or_zero(region, "end_us");
    info.confidence = confidence_value_or_one(region);
    info.shot_id = string_value(region, "shot_id");
    info.scene_id = string_value(region, "scene_id");
    info.frame_start = resolve_frame_ref(region, "frame_start", frame_lookup);
    info.frame_end = resolve_frame_ref(region, "frame_end", frame_lookup);

    if (region.contains("bbox_norm") && region["bbox_norm"].is_array()) {
      for (const auto& val : region["bbox_norm"]) {
        if (val.is_number()) {
          info.bbox_norm.push_back(val.get<double>());
        }
      }
    }

    groups_by_text[text_it->second].regions.push_back(std::move(info));
  }

  std::vector<EntityGroup> groups;
  groups.reserve(groups_by_text.size());
  for (auto& [text, group] : groups_by_text) {
    group.normalized_text = text;
    std::sort(group.regions.begin(), group.regions.end(),
              [](const TextRegionInfo& a, const TextRegionInfo& b) {
                if (a.start_us != b.start_us) {
                  return a.start_us < b.start_us;
                }
                return a.text_region_id < b.text_region_id;
              });
    groups.push_back(std::move(group));
  }

  std::sort(groups.begin(), groups.end(),
            [](const EntityGroup& a, const EntityGroup& b) {
              const std::int64_t a_start =
                  a.regions.empty() ? 0 : a.regions[0].start_us;
              const std::int64_t b_start =
                  b.regions.empty() ? 0 : b.regions[0].start_us;
              if (a_start != b_start) {
                return a_start < b_start;
              }
              return a.normalized_text < b.normalized_text;
            });

  return groups;
}

std::string deterministic_entity_id(std::size_t index) {
  return "entity_" + std::string(6 - std::min<std::size_t>(6, std::to_string(index + 1).length()), '0') +
         std::to_string(index + 1);
}

std::string deterministic_track_id(std::size_t index) {
  return "track_" + std::string(6 - std::min<std::size_t>(6, std::to_string(index + 1).length()), '0') +
         std::to_string(index + 1) + "_a";
}

double compute_average_screen_area(const EntityGroup& group) {
  double total = 0.0;
  std::size_t count = 0;
  for (const auto& region : group.regions) {
    if (region.bbox_norm.size() >= 4) {
      const double width = region.bbox_norm[2] - region.bbox_norm[0];
      const double height = region.bbox_norm[3] - region.bbox_norm[1];
      if (width > 0.0 && height > 0.0) {
        total += width * height;
        ++count;
      }
    }
  }
  return count > 0 ? total / static_cast<double>(count) : 0.0;
}

double compute_average_visibility(const EntityGroup& group,
                                  std::int64_t media_duration_us) {
  if (group.regions.empty() || media_duration_us <= 0) {
    return 0.0;
  }
  std::int64_t total_visible = 0;
  for (const auto& region : group.regions) {
    total_visible += (region.end_us - region.start_us);
  }
  return static_cast<double>(total_visible) / static_cast<double>(media_duration_us);
}

std::int64_t media_duration_from_timeline(const std::filesystem::path& staging_dir) {
  const auto shots = read_jsonl(staging_dir / "timeline" / "shots.jsonl");
  std::int64_t max_end = 0;
  for (const auto& shot : shots) {
    const std::int64_t end = int_value_or_zero(shot, "end_us");
    if (end > max_end) {
      max_end = end;
    }
  }
  if (max_end > 0) {
    return max_end;
  }
  const auto frames = read_jsonl(staging_dir / "timeline" / "frames.jsonl");
  for (const auto& frame : frames) {
    const std::int64_t ts = int_value_or_zero(frame, "timestamp_us");
    if (ts > max_end) {
      max_end = ts;
    }
  }
  return max_end;
}

nlohmann::json make_entity_record(std::size_t entity_index,
                                  const EntityGroup& group,
                                  const std::string& track_id,
                                  std::int64_t media_duration_us) {
  const std::string entity_id = deterministic_entity_id(entity_index);
  const std::int64_t first_seen = group.regions.front().start_us;
  const std::int64_t last_seen = group.regions.back().end_us;
  const double avg_area = compute_average_screen_area(group);
  const double avg_visibility = compute_average_visibility(group, media_duration_us);

  nlohmann::json record = {
      {"id", entity_id},
      {"entity_type", "unknown_region"},
      {"first_seen_us", first_seen},
      {"last_seen_us", last_seen},
      {"track_ids", nlohmann::json::array({track_id})},
      {"average_visibility", avg_visibility},
      {"average_screen_area", avg_area},
      {"processor_id", kEntityProcessorId},
  };

  nlohmann::json evidence = {
      {"source", "text/text_regions.jsonl"},
      {"evidence_type", "ocr_text_region"},
      {"normalized_text", group.normalized_text},
      {"region_count", group.regions.size()},
      {"region_ids", nlohmann::json::array()},
  };
  for (const auto& region : group.regions) {
    evidence["region_ids"].push_back(region.text_region_id);
  }
  record["evidence"] = std::move(evidence);

  return record;
}

nlohmann::json make_track_record(std::size_t entity_index,
                                 const EntityGroup& group) {
  const std::string entity_id = deterministic_entity_id(entity_index);
  const std::string track_id = deterministic_track_id(entity_index);
  const std::int64_t start_us = group.regions.front().start_us;
  const std::int64_t end_us = group.regions.back().end_us;
  const std::string start_frame = group.regions.front().frame_start;
  const std::string end_frame = group.regions.back().frame_end;

  double avg_confidence = 0.0;
  for (const auto& region : group.regions) {
    avg_confidence += region.confidence;
  }
  avg_confidence /= static_cast<double>(group.regions.size());

  nlohmann::json record = {
      {"id", track_id},
      {"entity_id", entity_id},
      {"start_us", start_us},
      {"end_us", end_us},
      {"region_count", group.regions.size()},
      {"lost_frame_count", 0},
      {"reacquired", false},
      {"confidence", avg_confidence},
      {"processor_id", kEntityProcessorId},
  };

  if (!start_frame.empty()) {
    record["start_frame_id"] = start_frame;
  }
  if (!end_frame.empty()) {
    record["end_frame_id"] = end_frame;
  }

  nlohmann::json evidence = {
      {"source", "text/text_regions.jsonl"},
      {"evidence_type", "ocr_text_region"},
      {"tracking_method", "text_content_match"},
      {"tracking_note", "Single-observation or text-content-matched track. "
                        "Cross-frame matching is based on identical normalized "
                        "text content, not visual tracking."},
  };
  record["evidence"] = std::move(evidence);

  return record;
}

nlohmann::json make_entity_processor_record(const EntityWriteSummary& summary) {
  return {
      {"id", kEntityProcessorId},
      {"name", "svp package entity writer"},
      {"version", "svp-package-entity-writer-v1"},
      {"input_refs", {
          "text/text_regions.jsonl",
          "text/text_observations.jsonl",
          "timeline/shots.jsonl",
          "timeline/frames.jsonl"
      }},
      {"output_refs", {
          "entities/entities.jsonl",
          "entities/entity_tracks.jsonl"
      }},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.entity.derive_from_text_regions"}},
      {"cache_keys", nlohmann::json::array()},
      {"entity_count", summary.entity_count},
      {"track_count", summary.track_count},
      {"skipped_missing_evidence", summary.skipped_missing_evidence},
      {"provenance_note", "Entities are derived from OCR text region evidence. "
                          "Entity type is 'unknown_region' because no object "
                          "recognition model is used. Labels are not assigned. "
                          "Tracks represent text-content-matched observations, "
                          "not robust visual tracking."},
  };
}

void append_processor_record(
    const std::filesystem::path& processors_path,
    const nlohmann::json& new_processor,
    EntityWriteSummary& summary) {
  std::map<std::string, nlohmann::json> processors_by_id;
  std::map<std::string, std::string> canonical_by_id;

  for (const nlohmann::json& processor : read_jsonl(processors_path)) {
    const std::string id = string_value(processor, "id");
    if (id.empty()) {
      continue;
    }
    const std::string canonical = processor.dump();
    const auto existing = canonical_by_id.find(id);
    if (existing != canonical_by_id.end()) {
      ++summary.duplicate_processors_merged;
      if (canonical >= existing->second) {
        continue;
      }
    }
    canonical_by_id[id] = canonical;
    processors_by_id[id] = processor;
  }

  const std::string id = string_value(new_processor, "id");
  if (!id.empty()) {
    processors_by_id[id] = new_processor;
    canonical_by_id[id] = new_processor.dump();
  }

  std::vector<nlohmann::json> processors;
  processors.reserve(processors_by_id.size());
  for (const auto& [pid, processor] : processors_by_id) {
    processors.push_back(processor);
  }
  write_jsonl(processors_path, processors);
  summary.processors_written = processors.size();
}

}  // namespace

EntityWriteSummary write_entity_artifacts(
    const std::filesystem::path& staging_dir) {
  EntityWriteSummary summary;

  std::size_t skipped_missing_evidence = 0;
  const std::vector<EntityGroup> groups =
      group_text_regions(staging_dir, skipped_missing_evidence);
  summary.skipped_missing_evidence = skipped_missing_evidence;

  const std::int64_t media_duration_us = media_duration_from_timeline(staging_dir);

  std::vector<nlohmann::json> entities;
  std::vector<nlohmann::json> tracks;
  entities.reserve(groups.size());
  tracks.reserve(groups.size());

  for (std::size_t i = 0; i < groups.size(); ++i) {
    const std::string track_id = deterministic_track_id(i);
    entities.push_back(make_entity_record(i, groups[i], track_id, media_duration_us));
    tracks.push_back(make_track_record(i, groups[i]));
  }

  const std::filesystem::path entities_path = staging_dir / "entities" / "entities.jsonl";
  const std::filesystem::path tracks_path = staging_dir / "entities" / "entity_tracks.jsonl";

  // Read existing entities/tracks (may have been written by visual entity tracker)
  auto existing_entities = read_jsonl(entities_path);
  auto existing_tracks = read_jsonl(tracks_path);

  // Merge: append text-based entities to existing visual entities
  for (auto& e : entities) {
    existing_entities.push_back(std::move(e));
  }
  for (auto& t : tracks) {
    existing_tracks.push_back(std::move(t));
  }

  write_jsonl(entities_path, existing_entities);
  write_jsonl(tracks_path, existing_tracks);

  summary.entities_written = true;
  summary.tracks_written = true;
  summary.entity_count = existing_entities.size();
  summary.track_count = existing_tracks.size();

  append_processor_record(
      staging_dir / "provenance" / "processors.jsonl",
      make_entity_processor_record(summary),
      summary);

  return summary;
}

EntityWriteSummary write_visual_entity_artifacts(
    const std::filesystem::path& staging_dir,
    const svp::vision::EntityTrackResult& tracker_result) {
  EntityWriteSummary summary;

  const std::filesystem::path entities_dir = staging_dir / "entities";
  const std::filesystem::path spatial_dir = staging_dir / "spatial";
  std::filesystem::create_directories(entities_dir);
  std::filesystem::create_directories(spatial_dir);

  // Read existing entities and tracks (from text-based entity writer)
  auto existing_entities = read_jsonl(entities_dir / "entities.jsonl");
  auto existing_tracks = read_jsonl(entities_dir / "entity_tracks.jsonl");

  // Append visual entities
  std::vector<nlohmann::json> all_entities = existing_entities;
  std::vector<nlohmann::json> all_tracks = existing_tracks;

  for (const auto& entity : tracker_result.entities) {
    nlohmann::json entity_record;
    entity_record["id"] = entity.entity_id;
    entity_record["entity_type"] = entity.entity_type;
    entity_record["first_seen_us"] = entity.first_seen_us;
    entity_record["last_seen_us"] = entity.last_seen_us;
    entity_record["average_visibility"] = entity.average_visibility;
    entity_record["average_screen_area"] = entity.average_screen_area;
    entity_record["track_ids"] = entity.track_ids;
    entity_record["evidence_sources"] = entity.evidence_sources;
    all_entities.push_back(entity_record);
  }

  for (const auto& track : tracker_result.tracks) {
    nlohmann::json track_record;
    track_record["id"] = track.track_id;
    track_record["entity_id"] = track.entity_id;
    track_record["start_us"] = track.start_us;
    track_record["end_us"] = track.end_us;
    track_record["start_frame_id"] = track.start_frame_id;
    track_record["end_frame_id"] = track.end_frame_id;
    track_record["region_count"] = track.region_count;
    track_record["lost_frame_count"] = track.lost_frame_count;
    track_record["reacquired"] = track.reacquired;
    track_record["confidence"] = track.confidence;
    track_record["tracking_method"] = track.tracking_method;
    track_record["candidate_source"] = track.candidate_source;
    all_tracks.push_back(track_record);
  }

  // Write merged entities and tracks
  write_jsonl(entities_dir / "entities.jsonl", all_entities);
  write_jsonl(entities_dir / "entity_tracks.jsonl", all_tracks);

  summary.entities_written = true;
  summary.tracks_written = true;
  summary.entity_count = all_entities.size();
  summary.track_count = all_tracks.size();

  // Write spatial regions per spec §14.2
  std::vector<nlohmann::json> region_records;
  for (const auto& region : tracker_result.regions) {
    nlohmann::json record;
    record["id"] = region.region_id;
    record["entity_id"] = region.entity_id;
    record["track_id"] = region.track_id;
    record["frame_id"] = region.frame_id;
    record["pts_us"] = region.timestamp_us;
    record["box_norm"] = {region.box_norm[0], region.box_norm[1],
                          region.box_norm[2], region.box_norm[3]};
    record["box_px"] = {region.box_px[0], region.box_px[1],
                        region.box_px[2], region.box_px[3]};
    record["centroid_norm"] = {region.centroid_norm[0], region.centroid_norm[1]};
    record["screen_area_ratio"] = region.screen_area_ratio;
    record["mask_ref"] = "mask_" + region.region_id;
    record["depth_ref"] = region.depth_ref;
    record["depth_summary"] = {
      {"median_inverse_depth", region.median_inverse_depth},
      {"near_percentile_10", region.near_percentile_10},
      {"far_percentile_90", region.far_percentile_90}
    };
    record["confidence"] = region.confidence;
    record["candidate_source"] = region.candidate_source;
    region_records.push_back(record);
  }

  write_jsonl(spatial_dir / "regions.jsonl", region_records);
  summary.regions_written = true;
  summary.region_count = region_records.size();

  // Write masks via mask_writer
  std::vector<svp::vision::MaskWriteEntry> mask_entries;
  for (const auto& region : tracker_result.regions) {
    if (region.mask_pixels.empty() || region.mask_width <= 0 || region.mask_height <= 0) {
      continue;
    }
    svp::vision::MaskWriteEntry entry;
    entry.mask_id = "mask_" + region.region_id;
    entry.entity_id = region.entity_id;
    entry.track_id = region.track_id;
    entry.region_id = region.region_id;
    entry.frame_id = region.frame_id;
    entry.timestamp_us = region.timestamp_us;
    entry.width = region.mask_width;
    entry.height = region.mask_height;
    entry.rle_data = svp::vision::encode_mask_rle(
        region.mask_pixels.data(), region.mask_width, region.mask_height);
    mask_entries.push_back(entry);
  }

  auto mask_summary = svp::vision::write_masks(staging_dir, mask_entries);
  summary.masks_written = !mask_entries.empty();
  summary.mask_count = mask_entries.size();

  // If no masks were written, ensure the required empty block file exists
  if (mask_entries.empty()) {
    const auto masks_block = staging_dir / "spatial" / "masks.blocks.svpmz";
    if (!std::filesystem::exists(masks_block)) {
      std::ofstream empty_block(masks_block, std::ios::binary);
    }
  }

  // Append processor record for visual entity tracker
  nlohmann::json processor_record;
  processor_record["id"] = tracker_result.processor_id;
  processor_record["name"] = "svp visual entity tracker";
  processor_record["version"] = "svp-visual-entity-tracker-v1";
  processor_record["input_refs"] = {
    "canonical_analysis_raster_frames",
    "spatial/depth.index.jsonl"
  };
  processor_record["output_refs"] = {
    "entities/entities.jsonl",
    "entities/entity_tracks.jsonl",
    "spatial/regions.jsonl",
    "spatial/masks.index.jsonl",
    "spatial/masks.blocks.svpmz"
  };
  processor_record["model_refs"] = tracker_result.model_refs;
  processor_record["task_ids"] = {
    "task.vision.visual_entity_tracking",
    "task.vision.mask_generation",
    "task.vision.spatial_region_generation"
  };
  processor_record["cache_keys"] = nlohmann::json::array();
  processor_record["status"] = "completed";
  processor_record["runtime"] = tracker_result.runtime;
  processor_record["execution_provider"] = tracker_result.execution_provider;
  processor_record["opencv_version"] = tracker_result.opencv_version;
  processor_record["confidence_calibration_status"] =
      tracker_result.confidence_calibration_status;
  processor_record["limitations"] = tracker_result.limitations_note;
  processor_record["parameters"] = tracker_result.parameters_json;

  append_processor_record(
      staging_dir / "provenance" / "processors.jsonl",
      processor_record,
      summary);

  return summary;
}

nlohmann::json entity_write_summary_to_json(const EntityWriteSummary& summary) {
  return {
      {"entities_written", summary.entities_written},
      {"tracks_written", summary.tracks_written},
      {"regions_written", summary.regions_written},
      {"masks_written", summary.masks_written},
      {"entity_count", summary.entity_count},
      {"track_count", summary.track_count},
      {"region_count", summary.region_count},
      {"mask_count", summary.mask_count},
      {"processors_written", summary.processors_written},
      {"duplicate_processors_merged", summary.duplicate_processors_merged},
      {"skipped_missing_evidence", summary.skipped_missing_evidence},
  };
}

}  // namespace svp::package
