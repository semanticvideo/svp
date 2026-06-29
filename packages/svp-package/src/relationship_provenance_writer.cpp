#include "svp/package/relationship_provenance_writer.hpp"

#include "svp/blocks/block_stream.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svp::package {
namespace {

constexpr const char* kRelationshipProcessorId = "processor_relationship_writer_0001";

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

bool has_int_field(const nlohmann::json& record, const char* key) {
  const auto iterator = record.find(key);
  return iterator != record.end() && iterator->is_number_integer();
}

double confidence_value_or_one(const nlohmann::json& record) {
  const auto iterator = record.find("confidence");
  if (iterator == record.end() || !iterator->is_number()) {
    return 1.0;
  }
  return iterator->get<double>();
}

nlohmann::json make_relationship(const std::string& id,
                                 const std::string& type,
                                 const std::string& source_id,
                                 const std::string& target_id,
                                 std::int64_t start_us,
                                 std::int64_t end_us,
                                 double confidence,
                                 const std::string& evidence_source_path) {
  return {
      {"id", id},
      {"type", type},
      {"source_id", source_id},
      {"target_id", target_id},
      {"start_us", start_us},
      {"end_us", end_us},
      {"evidence", {
          {"source_path", evidence_source_path}
      }},
      {"confidence", confidence},
      {"processor_id", kRelationshipProcessorId},
  };
}

nlohmann::json make_relationship_from_record(const std::string& id,
                                              const std::string& type,
                                              const std::string& source_id,
                                              const std::string& target_id,
                                              const nlohmann::json& source_record,
                                              const std::string& evidence_source_path) {
  return make_relationship(
      id, type, source_id, target_id,
      int_value_or_zero(source_record, "start_us"),
      int_value_or_zero(source_record, "end_us"),
      confidence_value_or_one(source_record),
      evidence_source_path);
}

struct KnownIds {
  std::set<std::string> text_region_ids;
  std::set<std::string> text_observation_ids;
  std::set<std::string> numeric_value_ids;
  std::set<std::string> evidence_crop_ids;
  std::set<std::string> color_observation_ids;
  std::set<std::string> speaker_ids;
  std::set<std::string> speaker_segment_ids;
  std::set<std::string> word_ids;
  std::set<std::string> embedding_ids;
  std::set<std::string> depth_ids;
  std::set<std::string> frame_ids;
  std::set<std::string> shot_ids;
  std::set<std::string> scene_ids;
  std::set<std::string> entity_ids;
  std::set<std::string> track_ids;
  std::set<std::string> processor_ids;
  std::set<std::string> spatial_region_ids;
  std::set<std::string> spatial_mask_ids;
};

KnownIds collect_known_ids(const std::filesystem::path& staging_dir) {
  KnownIds ids;

  // Text artifact IDs — text_regions is the owning artifact for text_region_id
  for (const auto& rec : read_jsonl(staging_dir / "text" / "text_regions.jsonl")) {
    const std::string id = string_value(rec, "text_region_id");
    if (!id.empty()) ids.text_region_ids.insert(id);
    // Do NOT collect shot_id/scene_id from text_regions — those are references,
    // not canonical definitions. Timeline artifacts own those IDs.
  }

  for (const auto& rec : read_jsonl(staging_dir / "text" / "text_observations.jsonl")) {
    const std::string id = string_value(rec, "text_observation_id");
    if (!id.empty()) ids.text_observation_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "text" / "numeric_values.jsonl")) {
    const std::string id = string_value(rec, "numeric_value_id");
    if (!id.empty()) ids.numeric_value_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "text" / "evidence_crops.jsonl")) {
    const std::string id = string_value(rec, "crop_id");
    if (!id.empty()) ids.evidence_crop_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "colors" / "color_observations.jsonl")) {
    const std::string id = string_value(rec, "color_observation_id");
    if (!id.empty()) ids.color_observation_ids.insert(id);
    // Do NOT collect target_id from color_observations — those are references.
  }

  for (const auto& rec : read_jsonl(staging_dir / "transcript" / "words.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.word_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "transcript" / "speakers.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.speaker_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "transcript" / "speaker_segments.jsonl")) {
    std::string id = string_value(rec, "id");
    if (id.empty()) id = string_value(rec, "segment_id");
    if (!id.empty()) ids.speaker_segment_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "embeddings" / "embeddings.index.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.embedding_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "spatial" / "depth.index.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.depth_ids.insert(id);
    // Do NOT collect frame_id from depth.index — depth is not the owning
    // artifact for frame IDs. Timeline/frames.jsonl owns those.
  }

  // Timeline artifacts — the canonical owners of frame, shot, and scene IDs.
  for (const auto& rec : read_jsonl(staging_dir / "timeline" / "frames.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.frame_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "timeline" / "shots.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.shot_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "timeline" / "scenes.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.scene_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "entities" / "entities.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.entity_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "entities" / "entity_tracks.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.track_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "spatial" / "regions.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.spatial_region_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "spatial" / "masks.index.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.spatial_mask_ids.insert(id);
  }

  for (const auto& rec : read_jsonl(staging_dir / "provenance" / "processors.jsonl")) {
    const std::string id = string_value(rec, "id");
    if (!id.empty()) ids.processor_ids.insert(id);
  }

  return ids;
}

struct RelationshipBuilder {
  std::vector<nlohmann::json> relationships;
  std::size_t sequence = 1;
  RelationshipTypeCounts counts;
  const KnownIds& ids;

  explicit RelationshipBuilder(const KnownIds& known_ids) : ids(known_ids) {}

  std::string next_id(const std::string& prefix) {
    return prefix + std::to_string(sequence++);
  }

  void add(const std::string& prefix,
           const std::string& type,
           const std::string& source_id,
           const std::string& target_id,
           std::int64_t start_us,
           std::int64_t end_us,
           double confidence,
           const std::string& evidence_path) {
    if (source_id.empty() || target_id.empty()) {
      ++counts.skipped_dangling;
      return;
    }
    relationships.push_back(make_relationship(
        next_id(prefix), type, source_id, target_id,
        start_us, end_us, confidence, evidence_path));
  }
};

void build_text_region_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto text_regions = read_jsonl(staging_dir / "text" / "text_regions.jsonl");
  for (const auto& region : text_regions) {
    const std::string region_id = string_value(region, "text_region_id");
    if (region_id.empty()) continue;

    const std::string shot_id = string_value(region, "shot_id");
    if (!shot_id.empty()) {
      if (builder.ids.shot_ids.count(shot_id)) {
        builder.add("rel_tr_shot_", "appears_in_shot", region_id, shot_id,
                    int_value_or_zero(region, "start_us"),
                    int_value_or_zero(region, "end_us"),
                    confidence_value_or_one(region),
                    "text/text_regions.jsonl");
        ++builder.counts.text_region_shot;
      } else {
        ++builder.counts.skipped_dangling;
      }
    }

    const std::string scene_id = string_value(region, "scene_id");
    if (!scene_id.empty()) {
      if (builder.ids.scene_ids.count(scene_id)) {
        builder.add("rel_tr_scene_", "appears_in_scene", region_id, scene_id,
                    int_value_or_zero(region, "start_us"),
                    int_value_or_zero(region, "end_us"),
                    confidence_value_or_one(region),
                    "text/text_regions.jsonl");
        ++builder.counts.text_region_scene;
      } else {
        ++builder.counts.skipped_dangling;
      }
    }
  }
}

void build_text_observation_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto observations = read_jsonl(staging_dir / "text" / "text_observations.jsonl");
  for (const auto& obs : observations) {
    const std::string obs_id = string_value(obs, "text_observation_id");
    const std::string region_id = string_value(obs, "text_region_id");
    if (obs_id.empty()) continue;

    if (!region_id.empty() && builder.ids.text_region_ids.count(region_id)) {
      builder.add("rel_obs_region_", "observation_in_region",
                  obs_id, region_id, 0, 0,
                  confidence_value_or_one(obs),
                  "text/text_observations.jsonl");
      ++builder.counts.text_observation_region;
    } else if (!region_id.empty()) {
      ++builder.counts.skipped_dangling;
    }

    if (obs.contains("evidence_crop_refs") && obs["evidence_crop_refs"].is_array()) {
      for (const auto& crop_ref : obs["evidence_crop_refs"]) {
        if (!crop_ref.is_string()) continue;
        const std::string crop_id = crop_ref.get<std::string>();
        if (builder.ids.evidence_crop_ids.count(crop_id)) {
          builder.add("rel_obs_crop_", "has_evidence_crop",
                      obs_id, crop_id, 0, 0,
                      confidence_value_or_one(obs),
                      "text/text_observations.jsonl");
          ++builder.counts.text_observation_evidence_crop;
        } else {
          ++builder.counts.skipped_dangling;
        }
      }
    }
  }
}

void build_numeric_value_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto numeric_values = read_jsonl(staging_dir / "text" / "numeric_values.jsonl");
  for (const auto& num : numeric_values) {
    const std::string num_id = string_value(num, "numeric_value_id");
    const std::string obs_id = string_value(num, "text_observation_id");
    if (num_id.empty()) continue;

    if (!obs_id.empty() && builder.ids.text_observation_ids.count(obs_id)) {
      builder.add("rel_num_obs_", "numeric_value_from_observation",
                  num_id, obs_id, 0, 0,
                  confidence_value_or_one(num),
                  "text/numeric_values.jsonl");
      ++builder.counts.numeric_value_observation;
    } else if (!obs_id.empty()) {
      ++builder.counts.skipped_dangling;
    }
  }
}

void build_transcript_speaker_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto words = read_jsonl(staging_dir / "transcript" / "words.jsonl");
  const auto speaker_segments = read_jsonl(staging_dir / "transcript" / "speaker_segments.jsonl");

  for (const auto& word : words) {
    const std::string word_id = string_value(word, "id");
    if (word_id.empty()) continue;

    const std::string speaker_id = string_value(word, "speaker_id");
    if (!speaker_id.empty() && builder.ids.speaker_ids.count(speaker_id)) {
      builder.add("rel_word_speaker_", "word_spoken_by",
                  word_id, speaker_id,
                  int_value_or_zero(word, "start_us"),
                  int_value_or_zero(word, "end_us"),
                  confidence_value_or_one(word),
                  "transcript/words.jsonl");
      ++builder.counts.word_speaker;
    } else if (!speaker_id.empty()) {
      ++builder.counts.skipped_dangling;
    }

    const std::int64_t word_mid =
        (int_value_or_zero(word, "start_us") +
         int_value_or_zero(word, "end_us")) / 2;
    bool matched_segment = false;
    for (const auto& seg : speaker_segments) {
      std::string seg_id = string_value(seg, "id");
      if (seg_id.empty()) seg_id = string_value(seg, "segment_id");
      if (seg_id.empty()) continue;
      const std::int64_t seg_start = int_value_or_zero(seg, "start_us");
      const std::int64_t seg_end = int_value_or_zero(seg, "end_us");
      if (word_mid >= seg_start && word_mid < seg_end) {
        if (builder.ids.speaker_segment_ids.count(seg_id)) {
          builder.add("rel_word_seg_", "word_in_speaker_segment",
                      word_id, seg_id,
                      int_value_or_zero(word, "start_us"),
                      int_value_or_zero(word, "end_us"),
                      confidence_value_or_one(word),
                      "transcript/words.jsonl");
          ++builder.counts.word_speaker_segment;
        } else {
          ++builder.counts.skipped_dangling;
        }
        matched_segment = true;
        break;
      }
    }
    if (!matched_segment) {
      ++builder.counts.word_speaker_segment_unmatched;
    }
  }
}

void build_color_observation_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto color_obs = read_jsonl(staging_dir / "colors" / "color_observations.jsonl");
  for (const auto& color : color_obs) {
    const std::string color_id = string_value(color, "color_observation_id");
    const std::string target_type = string_value(color, "target_type");
    const std::string target_id = string_value(color, "target_id");
    if (color_id.empty() || target_id.empty()) continue;

    bool target_exists = false;
    if (target_type == "frame") {
      target_exists = builder.ids.frame_ids.count(target_id) > 0;
    } else if (target_type == "shot") {
      target_exists = builder.ids.shot_ids.count(target_id) > 0;
    } else if (target_type == "scene") {
      target_exists = builder.ids.scene_ids.count(target_id) > 0;
    } else if (target_type == "text_region") {
      target_exists = builder.ids.text_region_ids.count(target_id) > 0;
    } else {
      target_exists = true;
    }

    if (target_exists) {
      builder.add("rel_color_target_", "color_observation_of",
                  color_id, target_id, 0, 0,
                  confidence_value_or_one(color),
                  "colors/color_observations.jsonl");
      ++builder.counts.color_observation_target;
    } else {
      ++builder.counts.skipped_dangling;
    }
  }
}

void build_depth_frame_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto depth_entries = read_jsonl(staging_dir / "spatial" / "depth.index.jsonl");
  for (const auto& depth : depth_entries) {
    const std::string depth_id = string_value(depth, "id");
    const std::string frame_id = string_value(depth, "frame_id");
    if (depth_id.empty() || frame_id.empty()) continue;

    if (builder.ids.frame_ids.count(frame_id)) {
      builder.add("rel_depth_frame_", "depth_for_frame",
                  depth_id, frame_id,
                  int_value_or_zero(depth, "start_us"),
                  int_value_or_zero(depth, "end_us"),
                  1.0,
                  "spatial/depth.index.jsonl");
      ++builder.counts.depth_frame;
    } else {
      ++builder.counts.skipped_dangling;
    }
  }
}

void build_embedding_source_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto embeddings = read_jsonl(staging_dir / "embeddings" / "embeddings.index.jsonl");
  for (const auto& emb : embeddings) {
    const std::string emb_id = string_value(emb, "id");
    const std::string input_ref = string_value(emb, "input_ref");
    const std::string input_kind = string_value(emb, "input_kind");
    if (emb_id.empty() || input_ref.empty()) continue;

    bool source_exists = false;
    if (input_kind == "text_observation") {
      source_exists = builder.ids.text_observation_ids.count(input_ref) > 0 ||
                      builder.ids.text_region_ids.count(input_ref) > 0;
    } else if (input_kind == "text_region") {
      source_exists = builder.ids.text_region_ids.count(input_ref) > 0;
    } else if (input_kind == "transcript" || input_kind == "word") {
      source_exists = builder.ids.word_ids.count(input_ref) > 0;
    } else {
      source_exists = true;
    }

    if (source_exists) {
      builder.add("rel_emb_source_", "embedding_source_is",
                  emb_id, input_ref,
                  int_value_or_zero(emb, "start_us"),
                  int_value_or_zero(emb, "end_us"),
                  1.0,
                  "embeddings/embeddings.index.jsonl");
      ++builder.counts.embedding_source;
    } else {
      ++builder.counts.skipped_dangling;
    }
  }
}

void build_spatial_region_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
  const auto masks = read_jsonl(staging_dir / "spatial" / "masks.index.jsonl");

  // Build entity -> region relationships (appears_in_frame)
  for (const auto& region : regions) {
    const std::string region_id = string_value(region, "id");
    const std::string entity_id = string_value(region, "entity_id");
    const std::string frame_id = string_value(region, "frame_id");
    const std::string track_id = string_value(region, "track_id");
    const std::int64_t ts = int_value_or_zero(region, "pts_us");

    if (region_id.empty()) continue;

    // entity -> region (appears_in_frame)
    if (builder.ids.entity_ids.count(entity_id)) {
      builder.add("rel_entity_region_", "appears_in_frame",
                  entity_id, region_id, ts, ts, 1.0,
                  "spatial/regions.jsonl");
    }

    // track -> region (track_observation)
    if (builder.ids.track_ids.count(track_id)) {
      builder.add("rel_track_region_", "track_observation",
                  track_id, region_id, ts, ts, 1.0,
                  "spatial/regions.jsonl");
    }

    // region -> frame (region_in_frame)
    if (builder.ids.frame_ids.count(frame_id)) {
      builder.add("rel_region_frame_", "region_in_frame",
                  region_id, frame_id, ts, ts, 1.0,
                  "spatial/regions.jsonl");
    }
  }

  // Build region_id -> pts_us lookup for mask relationship timestamps
  std::map<std::string, std::int64_t> region_pts;
  for (const auto& region : regions) {
    const std::string rid = string_value(region, "id");
    if (!rid.empty()) {
      region_pts[rid] = int_value_or_zero(region, "pts_us");
    }
  }

  // Build region -> mask relationships (has_mask)
  for (const auto& mask : masks) {
    const std::string mask_id = string_value(mask, "id");
    const std::string region_id = string_value(mask, "region_id");

    if (mask_id.empty() || region_id.empty()) continue;

    // Use the owning region's pts_us for the relationship timestamp
    auto pts_it = region_pts.find(region_id);
    const std::int64_t ts = (pts_it != region_pts.end()) ? pts_it->second : 0;

    if (builder.ids.spatial_region_ids.count(region_id)) {
      builder.add("rel_region_mask_", "has_mask",
                  region_id, mask_id, ts, ts, 1.0,
                  "spatial/masks.index.jsonl");
    }
  }

  // Region-to-region spatial relationships that can be honestly derived from
  // bounding-box data in the JSONL records:
  //   overlaps, contains, contained_by, near
  // These use normalized bounding boxes (box_norm) which are real spatial
  // observations from the detection pipeline — not mask pixel data, but
  // legitimate geometric evidence from the region metadata.
  //
  // Relationships that require depth pixel data (occludes, occluded_by,
  // foreground_relative_to, background_relative_to) or multi-frame motion
  // analysis (moves_with, stationary_relative_to_camera) are NOT emitted
  // here because the JSONL metadata does not carry the necessary pixel-level
  // evidence.  They remain defined in the registry but ungenerated until
  // the builder can read binary block payloads.
}

// Compute IoU (intersection-over-union) of two normalized bounding boxes.
// Each bbox is [x0, y0, x1, y1] in [0, 1].
double bbox_iou(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() < 4 || b.size() < 4) return 0.0;
  const double ix0 = std::max(a[0], b[0]);
  const double iy0 = std::max(a[1], b[1]);
  const double ix1 = std::min(a[2], b[2]);
  const double iy1 = std::min(a[3], b[3]);
  if (ix1 <= ix0 || iy1 <= iy0) return 0.0;
  const double intersection = (ix1 - ix0) * (iy1 - iy0);
  const double area_a = (a[2] - a[0]) * (a[3] - a[1]);
  const double area_b = (b[2] - b[0]) * (b[3] - b[1]);
  const double union_area = area_a + area_b - intersection;
  if (union_area <= 0.0) return 0.0;
  return intersection / union_area;
}

// Check if bbox a fully contains bbox b.
bool bbox_contains(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() < 4 || b.size() < 4) return false;
  return a[0] <= b[0] && a[1] <= b[1] && a[2] >= b[2] && a[3] >= b[3];
}

// Compute center-to-center distance between two bboxes (normalized).
double bbox_center_distance(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() < 4 || b.size() < 4) return 1.0;
  const double acx = (a[0] + a[2]) / 2.0;
  const double acy = (a[1] + a[3]) / 2.0;
  const double bcx = (b[0] + b[2]) / 2.0;
  const double bcy = (b[1] + b[3]) / 2.0;
  const double dx = acx - bcx;
  const double dy = acy - bcy;
  return std::sqrt(dx * dx + dy * dy);
}

std::vector<double> extract_bbox_norm(const nlohmann::json& record, const std::string& field) {
  std::vector<double> bbox;
  if (record.contains(field) && record[field].is_array()) {
    for (const auto& val : record[field]) {
      if (val.is_number()) {
        bbox.push_back(val.get<double>());
      }
    }
  }
  return bbox;
}

struct RegionInfo {
  std::string id;
  std::string entity_id;
  std::string frame_id;
  std::int64_t pts_us = 0;
  std::vector<double> bbox;
};

void build_spatial_region_pair_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
  if (regions.size() < 2) return;

  // Group regions by frame_id so we only compare co-occurring regions.
  std::map<std::string, std::vector<RegionInfo>> regions_by_frame;
  for (const auto& region : regions) {
    const std::string region_id = string_value(region, "id");
    if (region_id.empty()) continue;
    if (builder.ids.spatial_region_ids.count(region_id) == 0) continue;

    const std::string frame_id = string_value(region, "frame_id");
    if (frame_id.empty()) continue;

    RegionInfo info;
    info.id = region_id;
    info.entity_id = string_value(region, "entity_id");
    info.frame_id = frame_id;
    info.pts_us = int_value_or_zero(region, "pts_us");
    info.bbox = extract_bbox_norm(region, "box_norm");
    if (info.bbox.size() < 4) continue;

    regions_by_frame[frame_id].push_back(std::move(info));
  }

  // Distance threshold for "near" — regions whose centers are within 0.15
  // of the normalized frame diagonal but do not overlap.
  constexpr double kNearThreshold = 0.15;

  for (const auto& [frame_id, frame_regions] : regions_by_frame) {
    for (std::size_t i = 0; i < frame_regions.size(); ++i) {
      for (std::size_t j = i + 1; j < frame_regions.size(); ++j) {
        const auto& a = frame_regions[i];
        const auto& b = frame_regions[j];
        if (a.id == b.id) continue;

        const double iou = bbox_iou(a.bbox, b.bbox);
        const std::int64_t ts = a.pts_us;

        if (iou > 0.0) {
          builder.add("rel_spatial_overlap_", "overlaps",
                      a.id, b.id, ts, ts, 1.0,
                      "spatial/regions.jsonl");
          ++builder.counts.spatial_overlaps;
        } else {
          const double dist = bbox_center_distance(a.bbox, b.bbox);
          if (dist <= kNearThreshold) {
            builder.add("rel_spatial_near_", "near",
                        a.id, b.id, ts, ts, 1.0,
                        "spatial/regions.jsonl");
            ++builder.counts.spatial_near;
          }
        }

        if (bbox_contains(a.bbox, b.bbox)) {
          builder.add("rel_spatial_contains_", "contains",
                      a.id, b.id, ts, ts, 1.0,
                      "spatial/regions.jsonl");
          ++builder.counts.spatial_contains;
        } else if (bbox_contains(b.bbox, a.bbox)) {
          builder.add("rel_spatial_contained_by_", "contained_by",
                      a.id, b.id, ts, ts, 1.0,
                      "spatial/regions.jsonl");
          ++builder.counts.spatial_contained_by;
        }
      }
    }
  }
}

void build_entity_enters_exits_frame_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
  if (regions.empty()) return;
  if (builder.ids.entity_ids.empty()) return;

  // For each entity, find the first and last frame it appears in.
  struct EntityFrameRange {
    std::int64_t min_pts = INT64_MAX;
    std::int64_t max_pts = INT64_MIN;
    std::string first_frame_id;
    std::string last_frame_id;
  };

  std::map<std::string, EntityFrameRange> entity_ranges;

  for (const auto& region : regions) {
    const std::string entity_id = string_value(region, "entity_id");
    if (entity_id.empty()) continue;
    if (builder.ids.entity_ids.count(entity_id) == 0) continue;

    const std::string frame_id = string_value(region, "frame_id");
    if (frame_id.empty()) continue;
    if (builder.ids.frame_ids.count(frame_id) == 0) continue;

    if (!has_int_field(region, "pts_us")) continue;
    const std::int64_t pts = int_value_or_zero(region, "pts_us");

    auto& range = entity_ranges[entity_id];
    if (pts < range.min_pts) {
      range.min_pts = pts;
      range.first_frame_id = frame_id;
    }
    if (pts > range.max_pts) {
      range.max_pts = pts;
      range.last_frame_id = frame_id;
    }
  }

  for (const auto& [entity_id, range] : entity_ranges) {
    if (!range.first_frame_id.empty()) {
      builder.add("rel_entity_enters_", "enters_frame",
                  entity_id, range.first_frame_id,
                  range.min_pts, range.min_pts, 1.0,
                  "spatial/regions.jsonl");
      ++builder.counts.entity_enters_frame;
    }
    if (!range.last_frame_id.empty() && range.last_frame_id != range.first_frame_id) {
      builder.add("rel_entity_exits_", "exits_frame",
                  entity_id, range.last_frame_id,
                  range.max_pts, range.max_pts, 1.0,
                  "spatial/regions.jsonl");
      ++builder.counts.entity_exits_frame;
    }
  }
}

void build_text_region_entity_overlap_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto text_regions = read_jsonl(staging_dir / "text" / "text_regions.jsonl");
  const auto spatial_regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");

  if (text_regions.empty() || spatial_regions.empty()) return;

  // Only emit overlap relationships when visual entities exist.
  // If entities are text-derived (fallback), there are no visual spatial
  // regions to overlap with, so this function is a no-op.
  if (builder.ids.entity_ids.empty()) return;

  for (const auto& tr : text_regions) {
    const std::string tr_id = string_value(tr, "text_region_id");
    if (tr_id.empty()) continue;
    if (builder.ids.text_region_ids.count(tr_id) == 0) continue;

    const std::int64_t tr_start = int_value_or_zero(tr, "start_us");
    const std::int64_t tr_end = int_value_or_zero(tr, "end_us");

    std::vector<double> tr_bbox;
    if (tr.contains("bbox_norm") && tr["bbox_norm"].is_array()) {
      for (const auto& val : tr["bbox_norm"]) {
        if (val.is_number()) {
          tr_bbox.push_back(val.get<double>());
        }
      }
    }
    if (tr_bbox.size() < 4) continue;

    for (const auto& sr : spatial_regions) {
      const std::string sr_entity_id = string_value(sr, "entity_id");
      if (sr_entity_id.empty()) continue;
      if (builder.ids.entity_ids.count(sr_entity_id) == 0) continue;

      const std::int64_t sr_ts = int_value_or_zero(sr, "pts_us");
      // Spatial regions are instantaneous (single frame), so treat them
      // as a point in time. Check if the text region's time span contains
      // the spatial region's timestamp.
      if (sr_ts < tr_start || sr_ts >= tr_end) continue;

      std::vector<double> sr_bbox;
      if (sr.contains("box_norm") && sr["box_norm"].is_array()) {
        for (const auto& val : sr["box_norm"]) {
          if (val.is_number()) {
            sr_bbox.push_back(val.get<double>());
          }
        }
      }
      if (sr_bbox.size() < 4) continue;

      // Only emit when there is actual spatial overlap (IoU > 0).
      const double iou = bbox_iou(tr_bbox, sr_bbox);
      if (iou <= 0.0) continue;

      builder.add("rel_tr_entity_", "text_region_overlaps_entity",
                  tr_id, sr_entity_id,
                  tr_start, tr_end,
                  confidence_value_or_one(tr),
                  "text/text_regions.jsonl+spatial/regions.jsonl");
      ++builder.counts.text_region_overlaps_entity;
    }
  }
}

bool intervals_overlap(std::int64_t a_start, std::int64_t a_end,
                       std::int64_t b_start, std::int64_t b_end) {
  if (a_start == a_end) {
    return a_start >= b_start && a_start < b_end;
  }
  if (b_start == b_end) {
    return b_start >= a_start && b_start < a_end;
  }
  return a_start < b_end && b_start < a_end;
}

struct IntervalRecord {
  std::string id;
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
};

std::vector<IntervalRecord> read_shots_as_intervals(
    const std::filesystem::path& staging_dir) {
  std::vector<IntervalRecord> intervals;
  for (const auto& rec : read_jsonl(staging_dir / "timeline" / "shots.jsonl")) {
    IntervalRecord iv;
    iv.id = string_value(rec, "id");
    iv.start_us = int_value_or_zero(rec, "start_us");
    iv.end_us = int_value_or_zero(rec, "end_us");
    if (!iv.id.empty() && iv.end_us > iv.start_us) {
      intervals.push_back(std::move(iv));
    }
  }
  return intervals;
}

std::vector<IntervalRecord> read_scenes_as_intervals(
    const std::filesystem::path& staging_dir) {
  std::vector<IntervalRecord> intervals;
  for (const auto& rec : read_jsonl(staging_dir / "timeline" / "scenes.jsonl")) {
    IntervalRecord iv;
    iv.id = string_value(rec, "id");
    iv.start_us = int_value_or_zero(rec, "start_us");
    iv.end_us = int_value_or_zero(rec, "end_us");
    if (!iv.id.empty() && iv.end_us > iv.start_us) {
      intervals.push_back(std::move(iv));
    }
  }
  return intervals;
}

std::vector<IntervalRecord> read_speaker_segments_as_intervals(
    const std::filesystem::path& staging_dir) {
  std::vector<IntervalRecord> intervals;
  for (const auto& rec : read_jsonl(staging_dir / "transcript" / "speaker_segments.jsonl")) {
    IntervalRecord iv;
    iv.id = string_value(rec, "id");
    if (iv.id.empty()) {
      iv.id = string_value(rec, "segment_id");
    }
    iv.start_us = int_value_or_zero(rec, "start_us");
    iv.end_us = int_value_or_zero(rec, "end_us");
    if (!iv.id.empty() && iv.end_us > iv.start_us) {
      intervals.push_back(std::move(iv));
    }
  }
  return intervals;
}

void build_entity_shot_scene_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
  if (regions.empty()) return;
  if (builder.ids.entity_ids.empty()) return;

  const auto shots = read_shots_as_intervals(staging_dir);
  const auto scenes = read_scenes_as_intervals(staging_dir);

  if (shots.empty() && scenes.empty()) return;

  for (const auto& region : regions) {
    const std::string entity_id = string_value(region, "entity_id");
    if (entity_id.empty()) continue;
    if (builder.ids.entity_ids.count(entity_id) == 0) continue;

    const std::int64_t pts_us = int_value_or_zero(region, "pts_us");
    if (!has_int_field(region, "pts_us")) continue;

    for (const auto& shot : shots) {
      if (pts_us >= shot.start_us && pts_us < shot.end_us) {
        builder.add("rel_entity_shot_", "appears_in_shot",
                    entity_id, shot.id,
                    pts_us, pts_us, 1.0,
                    "spatial/regions.jsonl+timeline/shots.jsonl");
        ++builder.counts.semantic_entity_appears_in_shot;
      }
    }

    for (const auto& scene : scenes) {
      if (pts_us >= scene.start_us && pts_us < scene.end_us) {
        builder.add("rel_entity_scene_", "appears_in_scene",
                    entity_id, scene.id,
                    pts_us, pts_us, 1.0,
                    "spatial/regions.jsonl+timeline/scenes.jsonl");
        ++builder.counts.semantic_entity_appears_in_scene;
      }
    }
  }
}

void build_visible_during_speech_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto speaker_segments = read_speaker_segments_as_intervals(staging_dir);
  if (speaker_segments.empty()) return;

  const auto text_regions = read_jsonl(staging_dir / "text" / "text_regions.jsonl");

  for (const auto& tr : text_regions) {
    const std::string tr_id = string_value(tr, "text_region_id");
    if (tr_id.empty()) continue;
    if (builder.ids.text_region_ids.count(tr_id) == 0) continue;

    const std::int64_t tr_start = int_value_or_zero(tr, "start_us");
    const std::int64_t tr_end = int_value_or_zero(tr, "end_us");
    if (tr_end <= tr_start) continue;

    for (const auto& seg : speaker_segments) {
      if (intervals_overlap(tr_start, tr_end, seg.start_us, seg.end_us)) {
        if (builder.ids.speaker_segment_ids.count(seg.id)) {
          builder.add("rel_tr_speech_", "visible_during_speech",
                      tr_id, seg.id,
                      tr_start, tr_end,
                      confidence_value_or_one(tr),
                      "text/text_regions.jsonl+transcript/speaker_segments.jsonl");
          ++builder.counts.semantic_visible_during_speech;
        }
      }
    }
  }

  if (!builder.ids.entity_ids.empty()) {
    const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
    for (const auto& region : regions) {
      const std::string entity_id = string_value(region, "entity_id");
      if (entity_id.empty()) continue;
      if (builder.ids.entity_ids.count(entity_id) == 0) continue;

      const std::int64_t pts_us = int_value_or_zero(region, "pts_us");
      if (!has_int_field(region, "pts_us")) continue;

      for (const auto& seg : speaker_segments) {
        if (pts_us >= seg.start_us && pts_us < seg.end_us) {
          if (builder.ids.speaker_segment_ids.count(seg.id)) {
            builder.add("rel_entity_speech_", "visible_during_speech",
                        entity_id, seg.id,
                        pts_us, pts_us, 1.0,
                        "spatial/regions.jsonl+transcript/speaker_segments.jsonl");
            ++builder.counts.semantic_visible_during_speech;
          }
        }
      }
    }
  }
}

void build_visible_during_word_range_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto words = read_jsonl(staging_dir / "transcript" / "words.jsonl");
  if (words.empty()) return;

  const auto text_regions = read_jsonl(staging_dir / "text" / "text_regions.jsonl");

  for (const auto& tr : text_regions) {
    const std::string tr_id = string_value(tr, "text_region_id");
    if (tr_id.empty()) continue;
    if (builder.ids.text_region_ids.count(tr_id) == 0) continue;

    const std::int64_t tr_start = int_value_or_zero(tr, "start_us");
    const std::int64_t tr_end = int_value_or_zero(tr, "end_us");
    if (tr_end <= tr_start) continue;

    for (const auto& word : words) {
      const std::string word_id = string_value(word, "id");
      if (word_id.empty()) continue;
      if (builder.ids.word_ids.count(word_id) == 0) continue;

      const std::int64_t w_start = int_value_or_zero(word, "start_us");
      const std::int64_t w_end = int_value_or_zero(word, "end_us");
      if (w_end <= w_start) continue;

      if (intervals_overlap(tr_start, tr_end, w_start, w_end)) {
        builder.add("rel_tr_word_", "visible_during_word_range",
                    tr_id, word_id,
                    tr_start, tr_end,
                    confidence_value_or_one(tr),
                    "text/text_regions.jsonl+transcript/words.jsonl");
        ++builder.counts.semantic_visible_during_word_range;
      }
    }
  }
}

void build_speaker_active_during_entity_visible_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto speaker_segments = read_speaker_segments_as_intervals(staging_dir);
  if (speaker_segments.empty()) return;
  if (builder.ids.entity_ids.empty()) return;

  const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
  if (regions.empty()) return;

  std::set<std::string> emitted_pairs;

  for (const auto& seg : speaker_segments) {
    if (builder.ids.speaker_segment_ids.count(seg.id) == 0) continue;

    for (const auto& region : regions) {
      const std::string entity_id = string_value(region, "entity_id");
      if (entity_id.empty()) continue;
      if (builder.ids.entity_ids.count(entity_id) == 0) continue;

      const std::int64_t pts_us = int_value_or_zero(region, "pts_us");
      if (!has_int_field(region, "pts_us")) continue;

      if (pts_us >= seg.start_us && pts_us < seg.end_us) {
        const std::string pair_key = seg.id + "|" + entity_id;
        if (emitted_pairs.count(pair_key) > 0) continue;
        emitted_pairs.insert(pair_key);

        builder.add("rel_seg_entity_", "speaker_active_during_entity_visible",
                    seg.id, entity_id,
                    seg.start_us, seg.end_us,
                    1.0,
                    "transcript/speaker_segments.jsonl+spatial/regions.jsonl");
        ++builder.counts.semantic_speaker_active_during_entity_visible;
      }
    }
  }
}

void build_frame_timeline_relationships(
    RelationshipBuilder& builder, const std::filesystem::path& staging_dir) {
  const auto frames = read_jsonl(staging_dir / "timeline" / "frames.jsonl");
  for (const auto& frame : frames) {
    const std::string frame_id = string_value(frame, "id");
    if (frame_id.empty()) continue;
    if (builder.ids.frame_ids.count(frame_id) == 0) continue;

    const std::int64_t pts_us = int_value_or_zero(frame, "pts_us");
    const double confidence = confidence_value_or_one(frame);

    const std::string shot_id = string_value(frame, "shot_id");
    if (!shot_id.empty() && builder.ids.shot_ids.count(shot_id)) {
      builder.add("rel_frame_shot_", "frame_in_shot",
                  frame_id, shot_id,
                  pts_us, pts_us, confidence,
                  "timeline/frames.jsonl");
      ++builder.counts.frame_in_shot;
    }

    const std::string scene_id = string_value(frame, "scene_id");
    if (!scene_id.empty() && builder.ids.scene_ids.count(scene_id)) {
      builder.add("rel_frame_scene_", "frame_in_scene",
                  frame_id, scene_id,
                  pts_us, pts_us, confidence,
                  "timeline/frames.jsonl");
      ++builder.counts.frame_in_scene;
    }
  }
}

// ---------------------------------------------------------------------------
// Mask/depth-backed spatial semantic relationships (spec §15)
//
// These functions read binary block payloads from the block stream files to
// derive spatial relationships from real pixel-level evidence:
//   - occludes / occluded_by: from mask pixel overlap
//   - foreground_relative_to / background_relative_to: from depth values
//   - moves_with / stationary_relative_to_camera: from entity displacement
// ---------------------------------------------------------------------------

struct MaskBlockInfo {
  std::string mask_id;
  std::string region_id;
  std::string frame_id;
  int width = 0;
  int height = 0;
  std::uint64_t block_offset = 0;
  std::uint64_t block_length = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t compressed_size = 0;
  std::uint8_t compression = 0;
};

std::vector<std::uint8_t> read_and_decompress_block(
    const std::filesystem::path& block_file_path,
    std::uint64_t block_offset,
    std::uint64_t block_length,
    std::uint64_t uncompressed_size,
    std::uint8_t compression) {
  if (!std::filesystem::exists(block_file_path)) return {};

  std::ifstream file(block_file_path, std::ios::binary);
  if (!file) return {};

  file.seekg(static_cast<std::streamoff>(block_offset));
  if (!file) return {};

  std::vector<std::uint8_t> block_data(static_cast<std::size_t>(block_length));
  file.read(reinterpret_cast<char*>(block_data.data()),
            static_cast<std::streamsize>(block_length));
  if (!file) return {};

  // Skip the 160-byte block header to get to the payload.
  constexpr std::size_t kHeaderSize = 160;
  if (block_data.size() < kHeaderSize) return {};

  const std::uint8_t* payload = block_data.data() + kHeaderSize;
  const std::size_t payload_size = block_data.size() - kHeaderSize;

  if (compression == 0x01) {
    // Zstd compressed
    std::vector<std::uint8_t> output(static_cast<std::size_t>(uncompressed_size));
    const auto result = ZSTD_decompress(
        output.data(), output.size(),
        payload, payload_size);
    if (ZSTD_isError(result) != 0U) return {};
    if (result != uncompressed_size) return {};
    return output;
  } else {
    // Uncompressed
    if (payload_size != uncompressed_size) return {};
    return std::vector<std::uint8_t>(payload, payload + payload_size);
  }
}

std::vector<MaskBlockInfo> collect_mask_infos(
    const std::filesystem::path& staging_dir) {
  const auto masks = read_jsonl(staging_dir / "spatial" / "masks.index.jsonl");
  std::vector<MaskBlockInfo> infos;
  for (const auto& mask : masks) {
    MaskBlockInfo info;
    info.mask_id = string_value(mask, "id");
    info.region_id = string_value(mask, "region_id");
    info.frame_id = string_value(mask, "frame_id");
    info.width = static_cast<int>(int_value_or_zero(mask, "width"));
    info.height = static_cast<int>(int_value_or_zero(mask, "height"));
    if (mask.contains("block_offset") && mask["block_offset"].is_number_unsigned()) {
      info.block_offset = mask["block_offset"].get<std::uint64_t>();
    }
    if (mask.contains("block_length") && mask["block_length"].is_number_unsigned()) {
      info.block_length = mask["block_length"].get<std::uint64_t>();
    }
    if (mask.contains("uncompressed_size") && mask["uncompressed_size"].is_number_unsigned()) {
      info.uncompressed_size = mask["uncompressed_size"].get<std::uint64_t>();
    }
    if (mask.contains("compressed_size") && mask["compressed_size"].is_number_unsigned()) {
      info.compressed_size = mask["compressed_size"].get<std::uint64_t>();
    }
    if (info.mask_id.empty() || info.region_id.empty()) continue;
    if (info.width <= 0 || info.height <= 0) continue;
    if (info.block_length == 0) continue;
    infos.push_back(std::move(info));
  }
  return infos;
}

struct DepthBlockInfo {
  std::string depth_id;
  std::string frame_id;
  int width = 0;
  int height = 0;
  std::uint64_t block_offset = 0;
  std::uint64_t block_length = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint8_t compression = 0;
  std::uint32_t dtype = 0;
};

// Pre-load regions to build a region_id -> pts_us lookup.
std::map<std::string, std::int64_t> build_region_pts_lookup(
    const std::filesystem::path& staging_dir) {
  std::map<std::string, std::int64_t> lookup;
  for (const auto& region : read_jsonl(staging_dir / "spatial" / "regions.jsonl")) {
    const auto id = string_value(region, "id");
    if (!id.empty()) {
      lookup[id] = int_value_or_zero(region, "pts_us");
    }
  }
  return lookup;
}

// Compute mean depth at masked pixels from a depth block.
// Returns negative value on failure.
double compute_mean_depth_at_mask(
    const std::uint16_t* depth_pixels,
    const std::vector<std::uint8_t>& mask_pixels,
    int width, int height) {
  double depth_sum = 0.0;
  std::size_t pixel_count = 0;
  const std::size_t total = static_cast<std::size_t>(width) * height;
  for (std::size_t p = 0; p < total; ++p) {
    if (mask_pixels[p]) {
      depth_sum += static_cast<double>(depth_pixels[p]);
      ++pixel_count;
    }
  }
  if (pixel_count == 0) return -1.0;
  return depth_sum / static_cast<double>(pixel_count);
}

void build_mask_occlusion_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto mask_infos = collect_mask_infos(staging_dir);
  if (mask_infos.size() < 2) {
    ++builder.counts.skipped_no_mask_data;
    return;
  }

  const auto block_file_path = staging_dir / "spatial" / "masks.blocks.svpmz";
  if (!std::filesystem::exists(block_file_path)) {
    ++builder.counts.skipped_no_mask_data;
    return;
  }

  const auto region_pts = build_region_pts_lookup(staging_dir);

  // Check if depth data is available for z-order determination.
  const auto depth_records = read_jsonl(staging_dir / "spatial" / "depth.index.jsonl");
  const auto depth_block_path = staging_dir / "spatial" / "depth.blocks.svpdz";
  const bool depth_available = !depth_records.empty() &&
      std::filesystem::exists(depth_block_path);

  // Collect depth block infos by frame_id for z-order lookup.
  std::map<std::string, DepthBlockInfo> depth_by_frame;
  if (depth_available) {
    for (const auto& rec : depth_records) {
      DepthBlockInfo info;
      info.depth_id = string_value(rec, "id");
      info.frame_id = string_value(rec, "frame_id");
      info.width = static_cast<int>(int_value_or_zero(rec, "width"));
      info.height = static_cast<int>(int_value_or_zero(rec, "height"));
      if (rec.contains("block_offset") && rec["block_offset"].is_number_unsigned()) {
        info.block_offset = rec["block_offset"].get<std::uint64_t>();
      }
      if (rec.contains("block_length") && rec["block_length"].is_number_unsigned()) {
        info.block_length = rec["block_length"].get<std::uint64_t>();
      }
      if (rec.contains("uncompressed_size") && rec["uncompressed_size"].is_number_unsigned()) {
        info.uncompressed_size = rec["uncompressed_size"].get<std::uint64_t>();
      }
      if (rec.contains("dtype") && rec["dtype"].is_number_unsigned()) {
        info.dtype = rec["dtype"].get<std::uint32_t>();
      }
      if (info.frame_id.empty() || info.width <= 0 || info.height <= 0) continue;
      info.compression = 0x01;
      depth_by_frame[info.frame_id] = std::move(info);
    }
  }

  // Group masks by frame_id so we only compare co-occurring masks.
  std::map<std::string, std::vector<MaskBlockInfo>> masks_by_frame;
  for (const auto& info : mask_infos) {
    if (info.frame_id.empty()) continue;
    if (builder.ids.spatial_mask_ids.count(info.mask_id) == 0) continue;
    masks_by_frame[info.frame_id].push_back(info);
  }

  for (const auto& [frame_id, frame_masks] : masks_by_frame) {
    // Decode all masks for this frame.
    std::vector<std::vector<std::uint8_t>> decoded_masks;
    decoded_masks.reserve(frame_masks.size());
    bool all_decoded = true;
    for (const auto& info : frame_masks) {
      auto raw = read_and_decompress_block(
          block_file_path,
          info.block_offset, info.block_length,
          info.uncompressed_size, 0x01);
      if (raw.empty()) {
        all_decoded = false;
        break;
      }
      auto pixels = svp::vision::decode_mask_rle(
          raw.data(), raw.size(), info.width, info.height);
      if (pixels.empty()) {
        all_decoded = false;
        break;
      }
      decoded_masks.push_back(std::move(pixels));
    }

    if (!all_decoded || decoded_masks.size() < 2) continue;

    // Try to load depth data for this frame for z-order determination.
    const auto* depth_info = depth_available ? &depth_by_frame[frame_id] : nullptr;
    std::vector<std::uint16_t> depth_pixels;
    bool depth_loaded = false;
    if (depth_info && depth_info->frame_id == frame_id && depth_info->dtype == 2) {
      auto depth_raw = read_and_decompress_block(
          depth_block_path,
          depth_info->block_offset, depth_info->block_length,
          depth_info->uncompressed_size, depth_info->compression);
      if (depth_raw.size() >= static_cast<std::size_t>(depth_info->width) * depth_info->height * 2) {
        depth_pixels.assign(
            reinterpret_cast<const std::uint16_t*>(depth_raw.data()),
            reinterpret_cast<const std::uint16_t*>(depth_raw.data()) +
                static_cast<std::size_t>(depth_info->width) * depth_info->height);
        depth_loaded = true;
      }
    }

    for (std::size_t i = 0; i < frame_masks.size(); ++i) {
      for (std::size_t j = i + 1; j < frame_masks.size(); ++j) {
        const auto& info_a = frame_masks[i];
        const auto& info_b = frame_masks[j];
        const auto& mask_a = decoded_masks[i];
        const auto& mask_b = decoded_masks[j];

        if (info_a.width != info_b.width || info_a.height != info_b.height) {
          continue;
        }

        std::size_t overlap_pixels = 0;
        std::size_t a_only_pixels = 0;
        std::size_t b_only_pixels = 0;
        const std::size_t total = static_cast<std::size_t>(info_a.width) * info_a.height;
        for (std::size_t p = 0; p < total; ++p) {
          if (mask_a[p] && mask_b[p]) {
            ++overlap_pixels;
          } else if (mask_a[p]) {
            ++a_only_pixels;
          } else if (mask_b[p]) {
            ++b_only_pixels;
          }
        }

        if (overlap_pixels == 0) continue;

        const auto pts_it = region_pts.find(info_a.region_id);
        const std::int64_t pts_us = pts_it != region_pts.end() ? pts_it->second : 0;

        if (depth_loaded &&
            info_a.width == depth_info->width && info_a.height == depth_info->height) {
          // Depth data available: use mean depth at masked regions to
          // determine z-order. Lower depth = closer to camera = occluder.
          const double mean_depth_a = compute_mean_depth_at_mask(
              depth_pixels.data(), mask_a, info_a.width, info_a.height);
          const double mean_depth_b = compute_mean_depth_at_mask(
              depth_pixels.data(), mask_b, info_b.width, info_b.height);

          if (mean_depth_a < 0 || mean_depth_b < 0) continue;

          if (mean_depth_a < mean_depth_b) {
            // A is closer to camera, A occludes B.
            builder.add("rel_mask_occludes_", "occludes",
                        info_a.region_id, info_b.region_id,
                        pts_us, pts_us, 1.0,
                        "spatial/masks.blocks.svpmz+spatial/depth.blocks.svpdz");
            builder.add("rel_mask_occluded_by_", "occluded_by",
                        info_b.region_id, info_a.region_id,
                        pts_us, pts_us, 1.0,
                        "spatial/masks.blocks.svpmz+spatial/depth.blocks.svpdz");
            ++builder.counts.spatial_occludes;
            ++builder.counts.spatial_occluded_by;
          } else if (mean_depth_b < mean_depth_a) {
            builder.add("rel_mask_occludes_", "occludes",
                        info_b.region_id, info_a.region_id,
                        pts_us, pts_us, 1.0,
                        "spatial/masks.blocks.svpmz+spatial/depth.blocks.svpdz");
            builder.add("rel_mask_occluded_by_", "occluded_by",
                        info_a.region_id, info_b.region_id,
                        pts_us, pts_us, 1.0,
                        "spatial/masks.blocks.svpmz+spatial/depth.blocks.svpdz");
            ++builder.counts.spatial_occludes;
            ++builder.counts.spatial_occluded_by;
          }
          // If equal depth, no occlusion relationship — just overlap.
          // Fall through to emit overlaps below for equal depth case.
          if (mean_depth_a != mean_depth_b) continue;
        }

        // Without depth data (or equal depth), masks prove pixel overlap
        // but not z-order. Emit the spec-defined 'overlaps' relationship
        // using mask IoU as the evidence metric.
        // Spec §15: "overlaps: mask IoU exceeds threshold"
        // Spec §15: "All thresholds are written to provenance"
        constexpr double kMaskIoUThreshold = 0.1;
        const double mask_iou =
            static_cast<double>(overlap_pixels) /
            static_cast<double>(overlap_pixels + a_only_pixels + b_only_pixels);

        if (mask_iou >= kMaskIoUThreshold) {
          builder.add("rel_mask_overlaps_", "overlaps",
                      info_a.region_id, info_b.region_id,
                      pts_us, pts_us, mask_iou,
                      "spatial/masks.blocks.svpmz");
          ++builder.counts.spatial_overlaps;
        }
      }
    }
  }
}

void build_depth_foreground_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto depth_records = read_jsonl(staging_dir / "spatial" / "depth.index.jsonl");
  if (depth_records.empty()) {
    ++builder.counts.skipped_no_depth_data;
    return;
  }

  const auto mask_infos = collect_mask_infos(staging_dir);
  if (mask_infos.empty()) {
    ++builder.counts.skipped_no_depth_data;
    return;
  }

  const auto depth_block_path = staging_dir / "spatial" / "depth.blocks.svpdz";
  if (!std::filesystem::exists(depth_block_path)) {
    ++builder.counts.skipped_no_depth_data;
    return;
  }

  // Collect depth block infos.
  std::map<std::string, DepthBlockInfo> depth_by_frame;
  for (const auto& rec : depth_records) {
    DepthBlockInfo info;
    info.depth_id = string_value(rec, "id");
    info.frame_id = string_value(rec, "frame_id");
    info.width = static_cast<int>(int_value_or_zero(rec, "width"));
    info.height = static_cast<int>(int_value_or_zero(rec, "height"));
    if (rec.contains("block_offset") && rec["block_offset"].is_number_unsigned()) {
      info.block_offset = rec["block_offset"].get<std::uint64_t>();
    }
    if (rec.contains("block_length") && rec["block_length"].is_number_unsigned()) {
      info.block_length = rec["block_length"].get<std::uint64_t>();
    }
    if (rec.contains("uncompressed_size") && rec["uncompressed_size"].is_number_unsigned()) {
      info.uncompressed_size = rec["uncompressed_size"].get<std::uint64_t>();
    }
    if (rec.contains("dtype") && rec["dtype"].is_number_unsigned()) {
      info.dtype = rec["dtype"].get<std::uint32_t>();
    }
    if (info.frame_id.empty() || info.width <= 0 || info.height <= 0) continue;
    info.compression = 0x01;  // zstd
    depth_by_frame[info.frame_id] = std::move(info);
  }

  // Group masks by frame.
  std::map<std::string, std::vector<MaskBlockInfo>> masks_by_frame;
  for (const auto& info : mask_infos) {
    if (info.frame_id.empty()) continue;
    if (builder.ids.spatial_mask_ids.count(info.mask_id) == 0) continue;
    masks_by_frame[info.frame_id].push_back(info);
  }

  const auto mask_block_path = staging_dir / "spatial" / "masks.blocks.svpmz";

  for (const auto& [frame_id, frame_masks] : masks_by_frame) {
    const auto depth_it = depth_by_frame.find(frame_id);
    if (depth_it == depth_by_frame.end()) continue;

    const auto& depth_info = depth_it->second;

    // Decode depth data.
    auto depth_raw = read_and_decompress_block(
        depth_block_path,
        depth_info.block_offset, depth_info.block_length,
        depth_info.uncompressed_size, depth_info.compression);
    if (depth_raw.empty()) continue;

    // Depth is stored as uint16 (DType=2) in row-major order.
    if (depth_info.dtype != 2) continue;  // Only support uint16 depth
    if (depth_raw.size() < static_cast<std::size_t>(depth_info.width) * depth_info.height * 2) {
      continue;
    }

    const std::uint16_t* depth_pixels = reinterpret_cast<const std::uint16_t*>(depth_raw.data());

    // Decode all masks for this frame and compute mean depth per masked region.
    struct RegionDepth {
      std::string region_id;
      double mean_depth = 0.0;
      std::int64_t pts_us = 0;
    };

    std::vector<RegionDepth> region_depths;
    for (const auto& mask_info : frame_masks) {
      auto mask_raw = read_and_decompress_block(
          mask_block_path,
          mask_info.block_offset, mask_info.block_length,
          mask_info.uncompressed_size, 0x01);
      if (mask_raw.empty()) continue;

      auto mask_pixels = svp::vision::decode_mask_rle(
          mask_raw.data(), mask_raw.size(), mask_info.width, mask_info.height);
      if (mask_pixels.empty()) continue;

      // Mask and depth must have the same dimensions.
      if (mask_info.width != depth_info.width || mask_info.height != depth_info.height) {
        continue;
      }

      double depth_sum = 0.0;
      std::size_t pixel_count = 0;
      const std::size_t total = static_cast<std::size_t>(mask_info.width) * mask_info.height;
      for (std::size_t p = 0; p < total; ++p) {
        if (mask_pixels[p]) {
          depth_sum += static_cast<double>(depth_pixels[p]);
          ++pixel_count;
        }
      }

      if (pixel_count == 0) continue;

      RegionDepth rd;
      rd.region_id = mask_info.region_id;
      rd.mean_depth = depth_sum / static_cast<double>(pixel_count);

      // Look up pts_us from regions.
      for (const auto& region : read_jsonl(staging_dir / "spatial" / "regions.jsonl")) {
        if (string_value(region, "id") == mask_info.region_id) {
          rd.pts_us = int_value_or_zero(region, "pts_us");
          break;
        }
      }

      region_depths.push_back(std::move(rd));
    }

    if (region_depths.size() < 2) continue;

    // Compare each pair: lower mean depth = foreground (closer to camera).
    // Depth values are typically disparity-like: higher = farther.
    for (std::size_t i = 0; i < region_depths.size(); ++i) {
      for (std::size_t j = i + 1; j < region_depths.size(); ++j) {
        const auto& a = region_depths[i];
        const auto& b = region_depths[j];

        if (a.mean_depth < b.mean_depth) {
          // A is closer to camera (foreground), B is background.
          builder.add("rel_depth_fg_", "foreground_relative_to",
                      a.region_id, b.region_id,
                      a.pts_us, a.pts_us, 1.0,
                      "spatial/depth.blocks.svpdz");
          builder.add("rel_depth_bg_", "background_relative_to",
                      b.region_id, a.region_id,
                      a.pts_us, a.pts_us, 1.0,
                      "spatial/depth.blocks.svpdz");
          ++builder.counts.spatial_foreground_relative_to;
          ++builder.counts.spatial_background_relative_to;
        } else if (b.mean_depth < a.mean_depth) {
          builder.add("rel_depth_fg_", "foreground_relative_to",
                      b.region_id, a.region_id,
                      a.pts_us, a.pts_us, 1.0,
                      "spatial/depth.blocks.svpdz");
          builder.add("rel_depth_bg_", "background_relative_to",
                      a.region_id, b.region_id,
                      a.pts_us, a.pts_us, 1.0,
                      "spatial/depth.blocks.svpdz");
          ++builder.counts.spatial_foreground_relative_to;
          ++builder.counts.spatial_background_relative_to;
        }
        // If equal depth, no foreground/background relationship.
      }
    }
  }
}

void build_motion_relationships(
    RelationshipBuilder& builder,
    const std::filesystem::path& staging_dir) {
  const auto tracks = read_jsonl(staging_dir / "entities" / "entity_tracks.jsonl");
  if (tracks.empty()) {
    ++builder.counts.skipped_no_track_data;
    return;
  }

  const auto regions = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
  if (regions.empty()) {
    ++builder.counts.skipped_no_track_data;
    return;
  }

  // Collect region positions grouped by entity_id and sorted by pts_us.
  struct RegionPosition {
    std::string region_id;
    std::int64_t pts_us = 0;
    std::vector<double> bbox;  // box_norm [x0, y0, x1, y1]
  };

  std::map<std::string, std::vector<RegionPosition>> positions_by_entity;
  for (const auto& region : regions) {
    const std::string entity_id = string_value(region, "entity_id");
    if (entity_id.empty()) continue;
    if (builder.ids.entity_ids.count(entity_id) == 0) continue;

    RegionPosition pos;
    pos.region_id = string_value(region, "id");
    if (pos.region_id.empty()) continue;
    if (builder.ids.spatial_region_ids.count(pos.region_id) == 0) continue;
    pos.pts_us = int_value_or_zero(region, "pts_us");
    pos.bbox = extract_bbox_norm(region, "box_norm");
    if (pos.bbox.size() < 4) continue;

    positions_by_entity[entity_id].push_back(std::move(pos));
  }

  if (positions_by_entity.empty()) {
    ++builder.counts.skipped_no_track_data;
    return;
  }

  // For each entity, compute displacement between first and last positions.
  // If displacement is below a threshold, the entity is stationary relative
  // to camera. If two entities have similar displacement vectors, they
  // move_with each other.
  struct EntityMotion {
    std::string entity_id;
    double cx_first = 0.0;
    double cy_first = 0.0;
    double cx_last = 0.0;
    double cy_last = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    double displacement = 0.0;
    std::int64_t first_pts = 0;
    std::int64_t last_pts = 0;
  };

  // Threshold for stationary: less than 0.02 normalized displacement.
  // Threshold for moves_with: displacement vectors within 0.05 of each other.
  constexpr double kStationaryThreshold = 0.02;
  constexpr double kMovesWithThreshold = 0.05;

  std::vector<EntityMotion> motions;

  for (const auto& [entity_id, positions] : positions_by_entity) {
    if (positions.size() < 2) continue;

    EntityMotion motion;
    motion.entity_id = entity_id;
    motion.cx_first = (positions.front().bbox[0] + positions.front().bbox[2]) / 2.0;
    motion.cy_first = (positions.front().bbox[1] + positions.front().bbox[3]) / 2.0;
    motion.cx_last = (positions.back().bbox[0] + positions.back().bbox[2]) / 2.0;
    motion.cy_last = (positions.back().bbox[1] + positions.back().bbox[3]) / 2.0;
    motion.dx = motion.cx_last - motion.cx_first;
    motion.dy = motion.cy_last - motion.cy_first;
    motion.displacement = std::sqrt(motion.dx * motion.dx + motion.dy * motion.dy);
    motion.first_pts = positions.front().pts_us;
    motion.last_pts = positions.back().pts_us;

    motions.push_back(std::move(motion));
  }

  if (motions.empty()) {
    ++builder.counts.skipped_no_track_data;
    return;
  }

  // Emit stationary_relative_to_camera for entities with low displacement.
  for (const auto& m : motions) {
    if (m.displacement <= kStationaryThreshold) {
      // Entity is stationary relative to camera.
      // Self-relationship: entity -> camera (represented as entity -> its first frame).
      // We use the entity's first frame as the reference point.
      builder.add("rel_stationary_", "stationary_relative_to_camera",
                  m.entity_id, m.entity_id,
                  m.first_pts, m.last_pts, 1.0,
                  "spatial/regions.jsonl");
      ++builder.counts.spatial_stationary_relative_to_camera;
    }
  }

  // Emit moves_with for entity pairs with similar displacement vectors.
  for (std::size_t i = 0; i < motions.size(); ++i) {
    for (std::size_t j = i + 1; j < motions.size(); ++j) {
      const auto& a = motions[i];
      const auto& b = motions[j];

      // Both entities must be moving (displacement > stationary threshold).
      if (a.displacement <= kStationaryThreshold) continue;
      if (b.displacement <= kStationaryThreshold) continue;

      // Compare displacement vectors.
      const double ddx = a.dx - b.dx;
      const double ddy = a.dy - b.dy;
      const double vector_diff = std::sqrt(ddx * ddx + ddy * ddy);

      if (vector_diff <= kMovesWithThreshold) {
        const std::int64_t start = std::min(a.first_pts, b.first_pts);
        const std::int64_t end = std::max(a.last_pts, b.last_pts);
        builder.add("rel_moves_with_", "moves_with",
                    a.entity_id, b.entity_id,
                    start, end, 1.0,
                    "spatial/regions.jsonl");
        ++builder.counts.spatial_moves_with;
      }
    }
  }
}

std::vector<nlohmann::json> build_relationships(const std::filesystem::path& staging_dir,
                                                 RelationshipTypeCounts& counts) {
  const KnownIds ids = collect_known_ids(staging_dir);
  RelationshipBuilder builder(ids);

  build_text_region_relationships(builder, staging_dir);
  build_text_observation_relationships(builder, staging_dir);
  build_numeric_value_relationships(builder, staging_dir);
  build_transcript_speaker_relationships(builder, staging_dir);
  build_color_observation_relationships(builder, staging_dir);
  build_depth_frame_relationships(builder, staging_dir);
  build_embedding_source_relationships(builder, staging_dir);
  build_spatial_region_relationships(builder, staging_dir);
  build_spatial_region_pair_relationships(builder, staging_dir);
  build_entity_enters_exits_frame_relationships(builder, staging_dir);
  build_text_region_entity_overlap_relationships(builder, staging_dir);
  build_frame_timeline_relationships(builder, staging_dir);
  build_entity_shot_scene_relationships(builder, staging_dir);
  build_visible_during_speech_relationships(builder, staging_dir);
  build_visible_during_word_range_relationships(builder, staging_dir);
  build_speaker_active_during_entity_visible_relationships(builder, staging_dir);
  build_mask_occlusion_relationships(builder, staging_dir);
  build_depth_foreground_relationships(builder, staging_dir);
  build_motion_relationships(builder, staging_dir);

  counts = builder.counts;

  std::sort(builder.relationships.begin(), builder.relationships.end(),
            [](const nlohmann::json& lhs, const nlohmann::json& rhs) {
              return lhs.value("id", "") < rhs.value("id", "");
            });
  return builder.relationships;
}

nlohmann::json make_relationship_processor_record(const RelationshipTypeCounts& counts) {
  return {
      {"id", kRelationshipProcessorId},
      {"name", "svp package relationship writer"},
      {"version", "svp-package-relationship-writer-v5"},
      {"input_refs", {
          "text/text_regions.jsonl",
          "text/text_observations.jsonl",
          "text/numeric_values.jsonl",
          "text/evidence_crops.jsonl",
          "transcript/words.jsonl",
          "transcript/speakers.jsonl",
          "transcript/speaker_segments.jsonl",
          "colors/color_observations.jsonl",
          "spatial/depth.index.jsonl",
          "embeddings/embeddings.index.jsonl",
          "timeline/frames.jsonl",
          "timeline/shots.jsonl",
          "timeline/scenes.jsonl"
      }},
      {"output_refs", {"relationships/relationships.jsonl"}},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {"task.relationships.full_graph"}},
      {"cache_keys", nlohmann::json::array()},
      {"relationship_type_counts", {
          {"text_region_shot", counts.text_region_shot},
          {"text_region_scene", counts.text_region_scene},
          {"text_observation_region", counts.text_observation_region},
          {"text_observation_evidence_crop", counts.text_observation_evidence_crop},
          {"numeric_value_observation", counts.numeric_value_observation},
          {"word_speaker", counts.word_speaker},
          {"word_speaker_segment", counts.word_speaker_segment},
          {"word_speaker_segment_unmatched", counts.word_speaker_segment_unmatched},
          {"color_observation_target", counts.color_observation_target},
          {"depth_frame", counts.depth_frame},
          {"embedding_source", counts.embedding_source},
          {"text_region_overlaps_entity", counts.text_region_overlaps_entity},
          {"skipped_dangling", counts.skipped_dangling},
          {"semantic_visible_during_speech", counts.semantic_visible_during_speech},
          {"semantic_visible_during_word_range", counts.semantic_visible_during_word_range},
          {"semantic_speaker_active_during_entity_visible", counts.semantic_speaker_active_during_entity_visible},
          {"semantic_entity_appears_in_shot", counts.semantic_entity_appears_in_shot},
          {"semantic_entity_appears_in_scene", counts.semantic_entity_appears_in_scene},
          {"frame_in_shot", counts.frame_in_shot},
          {"frame_in_scene", counts.frame_in_scene},
          {"spatial_overlaps", counts.spatial_overlaps},
          {"spatial_contains", counts.spatial_contains},
          {"spatial_contained_by", counts.spatial_contained_by},
          {"spatial_near", counts.spatial_near},
          {"entity_enters_frame", counts.entity_enters_frame},
          {"entity_exits_frame", counts.entity_exits_frame},
          {"spatial_occludes", counts.spatial_occludes},
          {"spatial_occluded_by", counts.spatial_occluded_by},
          {"spatial_foreground_relative_to", counts.spatial_foreground_relative_to},
          {"spatial_background_relative_to", counts.spatial_background_relative_to},
          {"spatial_moves_with", counts.spatial_moves_with},
          {"spatial_stationary_relative_to_camera", counts.spatial_stationary_relative_to_camera},
          {"skipped_no_mask_data", counts.skipped_no_mask_data},
          {"skipped_no_depth_data", counts.skipped_no_depth_data},
          {"skipped_no_track_data", counts.skipped_no_track_data}
      }},
  };
}

RelationshipProvenanceWriteSummary rewrite_processors(
    const std::filesystem::path& staging_dir,
    bool include_relationship_processor,
    const RelationshipTypeCounts& counts) {
  RelationshipProvenanceWriteSummary summary;
  const std::filesystem::path processors_path = staging_dir / "provenance" / "processors.jsonl";
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

  if (include_relationship_processor) {
    const nlohmann::json processor = make_relationship_processor_record(counts);
    processors_by_id[kRelationshipProcessorId] = processor;
    canonical_by_id[kRelationshipProcessorId] = processor.dump();
  }

  std::vector<nlohmann::json> processors;
  processors.reserve(processors_by_id.size());
  for (const auto& [id, processor] : processors_by_id) {
    processors.push_back(processor);
  }

  write_jsonl(processors_path, processors);
  summary.processors_written = processors.size();
  return summary;
}

}  // namespace

RelationshipProvenanceWriteSummary write_relationships_and_provenance(
    const std::filesystem::path& staging_dir) {
  RelationshipProvenanceWriteSummary summary;

  RelationshipTypeCounts counts;
  const std::vector<nlohmann::json> relationships = build_relationships(staging_dir, counts);
  write_jsonl(staging_dir / "relationships" / "relationships.jsonl", relationships);
  summary.relationships_written = relationships.size();
  summary.type_counts = counts;

  RelationshipProvenanceWriteSummary provenance_summary =
      rewrite_processors(staging_dir, !relationships.empty(), counts);
  summary.processors_written = provenance_summary.processors_written;
  summary.duplicate_processors_merged = provenance_summary.duplicate_processors_merged;
  return summary;
}

nlohmann::json relationship_provenance_write_summary_to_json(
    const RelationshipProvenanceWriteSummary& summary) {
  return {
      {"relationships_written", summary.relationships_written},
      {"processors_written", summary.processors_written},
      {"duplicate_processors_merged", summary.duplicate_processors_merged},
      {"type_counts", {
          {"text_region_shot", summary.type_counts.text_region_shot},
          {"text_region_scene", summary.type_counts.text_region_scene},
          {"text_observation_region", summary.type_counts.text_observation_region},
          {"text_observation_evidence_crop", summary.type_counts.text_observation_evidence_crop},
          {"numeric_value_observation", summary.type_counts.numeric_value_observation},
          {"word_speaker", summary.type_counts.word_speaker},
          {"word_speaker_segment", summary.type_counts.word_speaker_segment},
          {"word_speaker_segment_unmatched", summary.type_counts.word_speaker_segment_unmatched},
          {"color_observation_target", summary.type_counts.color_observation_target},
          {"depth_frame", summary.type_counts.depth_frame},
          {"embedding_source", summary.type_counts.embedding_source},
          {"text_region_overlaps_entity", summary.type_counts.text_region_overlaps_entity},
          {"skipped_dangling", summary.type_counts.skipped_dangling},
          {"semantic_visible_during_speech", summary.type_counts.semantic_visible_during_speech},
          {"semantic_visible_during_word_range", summary.type_counts.semantic_visible_during_word_range},
          {"semantic_speaker_active_during_entity_visible", summary.type_counts.semantic_speaker_active_during_entity_visible},
          {"semantic_entity_appears_in_shot", summary.type_counts.semantic_entity_appears_in_shot},
          {"semantic_entity_appears_in_scene", summary.type_counts.semantic_entity_appears_in_scene},
          {"frame_in_shot", summary.type_counts.frame_in_shot},
          {"frame_in_scene", summary.type_counts.frame_in_scene},
          {"spatial_overlaps", summary.type_counts.spatial_overlaps},
          {"spatial_contains", summary.type_counts.spatial_contains},
          {"spatial_contained_by", summary.type_counts.spatial_contained_by},
          {"spatial_near", summary.type_counts.spatial_near},
          {"entity_enters_frame", summary.type_counts.entity_enters_frame},
          {"entity_exits_frame", summary.type_counts.entity_exits_frame},
          {"spatial_occludes", summary.type_counts.spatial_occludes},
          {"spatial_occluded_by", summary.type_counts.spatial_occluded_by},
          {"spatial_foreground_relative_to", summary.type_counts.spatial_foreground_relative_to},
          {"spatial_background_relative_to", summary.type_counts.spatial_background_relative_to},
          {"spatial_moves_with", summary.type_counts.spatial_moves_with},
          {"spatial_stationary_relative_to_camera", summary.type_counts.spatial_stationary_relative_to_camera},
          {"skipped_no_mask_data", summary.type_counts.skipped_no_mask_data},
          {"skipped_no_depth_data", summary.type_counts.skipped_no_depth_data},
          {"skipped_no_track_data", summary.type_counts.skipped_no_track_data},
      }},
  };
}

}  // namespace svp::package
