#include "svp/package/relationship_provenance_writer.hpp"

#include <algorithm>
#include <cmath>
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
    const std::string id = string_value(rec, "id");
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
      const std::string seg_id = string_value(seg, "id");
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

  // Build region -> mask relationships (has_mask)
  for (const auto& mask : masks) {
    const std::string mask_id = string_value(mask, "id");
    const std::string region_id = string_value(mask, "region_id");
    const std::int64_t ts = 0;

    if (mask_id.empty() || region_id.empty()) continue;

    if (builder.ids.spatial_region_ids.count(region_id)) {
      builder.add("rel_region_mask_", "has_mask",
                  region_id, mask_id, ts, ts, 1.0,
                  "spatial/masks.index.jsonl");
    }
  }

  // Region-to-region visual spatial relationships (overlaps, contains, occludes)
  // per §20.8 require mask-based intersection and depth comparison.
  // These are not yet implemented because the relationship builder operates on
  // JSONL records which do not carry mask pixel data.  Emitting box-based
  // approximations under spec-named relationship types would be dishonest.
  // TODO: implement mask-based spatial relationships when mask block reading
  // is available in the relationship builder.
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
      {"version", "svp-package-relationship-writer-v3"},
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
          {"skipped_dangling", counts.skipped_dangling}
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
          {"skipped_dangling", summary.type_counts.skipped_dangling},
      }},
  };
}

}  // namespace svp::package
