#include "svp/query/traversal.hpp"

#include "svp/query/query_reader.hpp"
#include "svp/package/relationship_type_policy.hpp"

#include <algorithm>
#include <array>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_set>

namespace svp::query {
namespace {

struct CatalogLayerDef {
  std::string_view entry;
  std::array<std::string_view, 3> id_fields;
};

constexpr std::array<CatalogLayerDef, 17> kCatalogLayers{{
    {"transcript/words.jsonl",              {"id", "word_id", ""}},
    {"transcript/speakers.jsonl",           {"id", "speaker_id", ""}},
    {"transcript/speaker_segments.jsonl",   {"segment_id", "id", ""}},
    {"text/text_regions.jsonl",             {"text_region_id", "id", ""}},
    {"text/text_observations.jsonl",        {"text_observation_id", "id", ""}},
    {"text/numeric_values.jsonl",           {"numeric_value_id", "id", ""}},
    {"text/evidence_crops.jsonl",           {"crop_id", "id", ""}},
    {"colors/color_observations.jsonl",     {"color_observation_id", "id", ""}},
    {"timeline/frames.jsonl",               {"id", "frame_id", ""}},
    {"timeline/shots.jsonl",                {"id", "shot_id", ""}},
    {"timeline/scenes.jsonl",               {"id", "scene_id", ""}},
    {"entities/entities.jsonl",             {"id", "entity_id", ""}},
    {"entities/entity_tracks.jsonl",        {"id", "track_id", ""}},
    {"spatial/regions.jsonl",               {"region_id", "id", ""}},
    {"spatial/masks.index.jsonl",           {"mask_id", "block_id", "id"}},
    {"spatial/depth.index.jsonl",           {"depth_frame_id", "block_id", "id"}},
    {"embeddings/embeddings.index.jsonl",   {"embedding_id", "embedding_set_id", "id"}},
}};

std::string extract_id(const nlohmann::json& record,
                       const std::array<std::string_view, 3>& id_fields) {
  for (const auto field : id_fields) {
    if (field.empty()) continue;
    const auto it = record.find(field);
    if (it != record.end() && it->is_string()) {
      const auto val = it->get<std::string>();
      if (!val.empty()) return val;
    }
  }
  return {};
}

std::string json_string_val(const nlohmann::json& j, std::string_view key) {
  if (!j.is_object()) return {};
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

nlohmann::json make_word_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "word";
  if (record.contains("text")) summary["text"] = record["text"];
  if (record.contains("start_us")) summary["start_us"] = record["start_us"];
  if (record.contains("end_us")) summary["end_us"] = record["end_us"];
  if (record.contains("speaker_id")) summary["speaker_id"] = record["speaker_id"];
  return summary;
}

nlohmann::json make_speaker_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "speaker";
  if (record.contains("display_name")) summary["display_name"] = record["display_name"];
  if (record.contains("total_speech_us")) summary["total_speech_us"] = record["total_speech_us"];
  return summary;
}

nlohmann::json make_speaker_segment_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "speaker_segment";
  if (record.contains("speaker_id")) summary["speaker_id"] = record["speaker_id"];
  if (record.contains("start_us")) summary["start_us"] = record["start_us"];
  if (record.contains("end_us")) summary["end_us"] = record["end_us"];
  return summary;
}

nlohmann::json make_text_region_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "text_region";
  if (record.contains("observation_type")) summary["observation_type"] = record["observation_type"];
  if (record.contains("start_us")) summary["start_us"] = record["start_us"];
  if (record.contains("end_us")) summary["end_us"] = record["end_us"];
  if (record.contains("shot_id")) summary["shot_id"] = record["shot_id"];
  return summary;
}

nlohmann::json make_text_observation_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "text_observation";
  if (record.contains("raw_text")) summary["raw_text"] = record["raw_text"];
  if (record.contains("normalized_text")) summary["normalized_text"] = record["normalized_text"];
  if (record.contains("confidence")) summary["confidence"] = record["confidence"];
  if (record.contains("text_region_id")) summary["text_region_id"] = record["text_region_id"];
  return summary;
}

nlohmann::json make_numeric_value_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "numeric_value";
  if (record.contains("raw_text")) summary["raw_text"] = record["raw_text"];
  if (record.contains("numeric_value")) summary["numeric_value"] = record["numeric_value"];
  if (record.contains("unit")) summary["unit"] = record["unit"];
  if (record.contains("number_kind")) summary["number_kind"] = record["number_kind"];
  return summary;
}

nlohmann::json make_evidence_crop_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "evidence_crop";
  if (record.contains("crop_file_path")) summary["crop_file_path"] = record["crop_file_path"];
  if (record.contains("crop_size_bytes")) summary["crop_size_bytes"] = record["crop_size_bytes"];
  if (record.contains("image_format")) summary["image_format"] = record["image_format"];
  return summary;
}

nlohmann::json make_color_observation_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "color_observation";
  if (record.contains("dominant_bucket")) summary["dominant_bucket"] = record["dominant_bucket"];
  if (record.contains("target_type")) summary["target_type"] = record["target_type"];
  if (record.contains("target_id")) summary["target_id"] = record["target_id"];
  if (record.contains("coverage_total")) summary["coverage_total"] = record["coverage_total"];
  return summary;
}

nlohmann::json make_frame_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "frame";
  if (record.contains("pts_us")) summary["pts_us"] = record["pts_us"];
  if (record.contains("shot_id")) summary["shot_id"] = record["shot_id"];
  return summary;
}

nlohmann::json make_shot_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "shot";
  if (record.contains("start_us")) summary["start_us"] = record["start_us"];
  if (record.contains("end_us")) summary["end_us"] = record["end_us"];
  return summary;
}

nlohmann::json make_scene_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "scene";
  if (record.contains("start_us")) summary["start_us"] = record["start_us"];
  if (record.contains("end_us")) summary["end_us"] = record["end_us"];
  return summary;
}

nlohmann::json make_entity_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "entity";
  if (record.contains("entity_type")) summary["entity_type"] = record["entity_type"];
  if (record.contains("label")) summary["label"] = record["label"];
  return summary;
}

nlohmann::json make_entity_track_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "entity_track";
  if (record.contains("entity_id")) summary["entity_id"] = record["entity_id"];
  if (record.contains("start_us")) summary["start_us"] = record["start_us"];
  if (record.contains("end_us")) summary["end_us"] = record["end_us"];
  return summary;
}

nlohmann::json make_region_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "region";
  if (record.contains("frame_id")) summary["frame_id"] = record["frame_id"];
  return summary;
}

nlohmann::json make_generic_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "unknown";
  return summary;
}

nlohmann::json make_summary_for_layer(const std::string& source_layer,
                                       const nlohmann::json& record) {
  if (source_layer == "transcript/words.jsonl") return make_word_summary(record);
  if (source_layer == "transcript/speakers.jsonl") return make_speaker_summary(record);
  if (source_layer == "transcript/speaker_segments.jsonl") return make_speaker_segment_summary(record);
  if (source_layer == "text/text_regions.jsonl") return make_text_region_summary(record);
  if (source_layer == "text/text_observations.jsonl") return make_text_observation_summary(record);
  if (source_layer == "text/numeric_values.jsonl") return make_numeric_value_summary(record);
  if (source_layer == "text/evidence_crops.jsonl") return make_evidence_crop_summary(record);
  if (source_layer == "colors/color_observations.jsonl") return make_color_observation_summary(record);
  if (source_layer == "timeline/frames.jsonl") return make_frame_summary(record);
  if (source_layer == "timeline/shots.jsonl") return make_shot_summary(record);
  if (source_layer == "timeline/scenes.jsonl") return make_scene_summary(record);
  if (source_layer == "entities/entities.jsonl") return make_entity_summary(record);
  if (source_layer == "entities/entity_tracks.jsonl") return make_entity_track_summary(record);
  if (source_layer == "spatial/regions.jsonl") return make_region_summary(record);
  return make_generic_summary(record);
}

struct GraphEdge {
  std::string relationship_id;
  std::string relationship_type;
  std::string relationship_class;
  std::string source_id;
  std::string target_id;
  nlohmann::json record;
};

struct GraphNode {
  std::vector<std::size_t> outgoing;
  std::vector<std::size_t> incoming;
};

bool class_matches(const std::string& edge_class,
                   const std::string& class_filter) {
  if (class_filter == "all" || class_filter.empty()) return true;
  return edge_class == class_filter;
}

}  // namespace

const CatalogEntry* ObjectCatalog::find(const std::string& object_id) const {
  const auto it = entries.find(object_id);
  if (it == entries.end()) return nullptr;
  return &it->second;
}

nlohmann::json ObjectCatalog::node_summary(const std::string& object_id) const {
  const auto* entry = find(object_id);
  if (!entry) return {};
  return make_summary_for_layer(entry->source_layer, entry->record);
}

ObjectCatalog build_object_catalog(const std::filesystem::path& package_path) {
  ObjectCatalog catalog;

  for (const auto& layer : kCatalogLayers) {
    const auto jsonl = read_jsonl_entry(package_path, std::string{layer.entry});
    if (!jsonl.readable) continue;

    for (const auto& record : jsonl.records) {
      const auto id = extract_id(record, layer.id_fields);
      if (id.empty()) continue;

      CatalogEntry entry;
      entry.object_id = id;
      entry.source_layer = std::string{layer.entry};
      entry.record = record;
      catalog.entries[id] = std::move(entry);
    }
  }

  return catalog;
}

TraversalResult traverse_relationships(
    const std::filesystem::path& package_path,
    const TraversalOptions& options) {
  TraversalResult result;
  result.start_id = options.start_id;
  result.requested_depth = options.max_depth;

  if (options.start_id.empty()) {
    result.error_message = "start_id is required for traversal";
    return result;
  }

  const auto jsonl = read_jsonl_entry(package_path, "relationships/relationships.jsonl");
  if (!jsonl.present) {
    result.error_message = "relationships file not present";
    return result;
  }
  if (!jsonl.readable) {
    result.error_message = jsonl.error_message;
    return result;
  }

  std::vector<GraphEdge> edges;
  std::unordered_map<std::string, GraphNode> nodes;

  for (const auto& record : jsonl.records) {
    const auto type = json_string_val(record, "type");
    const auto source_id = json_string_val(record, "source_id");
    const auto target_id = json_string_val(record, "target_id");
    const auto rel_id = json_string_val(record, "id");

    if (source_id.empty() || target_id.empty()) continue;

    const auto cls = svp::package::classify_relationship_type(type);
    const auto class_str = std::string{svp::package::relationship_class_to_string(cls)};

    if (!class_matches(class_str, options.class_filter)) continue;

    if (options.type_filter.has_value() && type != *options.type_filter) continue;

    const auto edge_idx = edges.size();
    GraphEdge edge;
    edge.relationship_id = rel_id;
    edge.relationship_type = type;
    edge.relationship_class = class_str;
    edge.source_id = source_id;
    edge.target_id = target_id;
    edge.record = record;
    edges.push_back(std::move(edge));

    nodes[source_id].outgoing.push_back(edge_idx);
    nodes[target_id].incoming.push_back(edge_idx);
  }

  if (nodes.find(options.start_id) == nodes.end() &&
      options.start_id != result.start_id) {
    // start_id not in graph — still try, it may have no edges
  }

  ObjectCatalog catalog = build_object_catalog(package_path);

  std::unordered_set<std::string> visited;
  std::queue<std::pair<std::string, int>> frontier;
  frontier.push({options.start_id, 0});
  visited.insert(options.start_id);

  TraversalNode start_node;
  start_node.object_id = options.start_id;
  start_node.depth = 0;
  start_node.resolved = catalog.find(options.start_id) != nullptr;
  start_node.summary = catalog.node_summary(options.start_id);
  result.nodes.push_back(std::move(start_node));

  bool limit_hit = false;

  while (!frontier.empty() && !limit_hit) {
    auto [current_id, depth] = frontier.front();
    frontier.pop();

    if (depth >= options.max_depth) continue;

    const auto node_it = nodes.find(current_id);
    if (node_it == nodes.end()) continue;

    const auto collect_edges = [&](const std::vector<std::size_t>& edge_indices,
                                    const std::string& direction) {
      for (const auto idx : edge_indices) {
        if (limit_hit) break;
        if (result.edges.size() >= options.limit) {
          limit_hit = true;
          break;
        }

        const auto& edge = edges[idx];
        const auto neighbor_id = (direction == "outgoing")
            ? edge.target_id : edge.source_id;

        TraversalEdge tedge;
        tedge.relationship_id = edge.relationship_id;
        tedge.relationship_type = edge.relationship_type;
        tedge.relationship_class = edge.relationship_class;
        tedge.source_id = edge.source_id;
        tedge.target_id = edge.target_id;
        tedge.direction = direction;
        tedge.depth = depth + 1;
        tedge.record = edge.record;
        result.edges.push_back(std::move(tedge));

        if (visited.find(neighbor_id) == visited.end()) {
          visited.insert(neighbor_id);

          TraversalNode tnode;
          tnode.object_id = neighbor_id;
          tnode.depth = depth + 1;
          tnode.resolved = catalog.find(neighbor_id) != nullptr;
          tnode.summary = catalog.node_summary(neighbor_id);
          result.nodes.push_back(std::move(tnode));

          frontier.push({neighbor_id, depth + 1});
        }
      }
    };

    if (options.direction == TraversalDirection::Outgoing ||
        options.direction == TraversalDirection::Both) {
      collect_edges(node_it->second.outgoing, "outgoing");
    }
    if (options.direction == TraversalDirection::Incoming ||
        options.direction == TraversalDirection::Both) {
      collect_edges(node_it->second.incoming, "incoming");
    }
  }

  for (const auto& node : result.nodes) {
    if (!node.resolved) {
      result.missing_object_ids.push_back(node.object_id);
    }
  }

  result.visited_node_count = result.nodes.size();
  result.edge_count = result.edges.size();
  result.limit_applied = limit_hit;

  return result;
}

nlohmann::json traversal_result_to_json(const TraversalResult& result) {
  nlohmann::json nodes_arr = nlohmann::json::array();
  for (const auto& node : result.nodes) {
    nlohmann::json n = {
        {"object_id", node.object_id},
        {"depth", node.depth},
        {"resolved", node.resolved},
    };
    if (!node.summary.is_null()) {
      n["summary"] = node.summary;
    }
    nodes_arr.push_back(std::move(n));
  }

  nlohmann::json edges_arr = nlohmann::json::array();
  for (const auto& edge : result.edges) {
    nlohmann::json e = {
        {"relationship_id", edge.relationship_id},
        {"relationship_type", edge.relationship_type},
        {"relationship_class", edge.relationship_class},
        {"source_id", edge.source_id},
        {"target_id", edge.target_id},
        {"direction", edge.direction},
        {"depth", edge.depth},
    };
    edges_arr.push_back(std::move(e));
  }

  nlohmann::json out = {
      {"start_id", result.start_id},
      {"requested_depth", result.requested_depth},
      {"visited_node_count", result.visited_node_count},
      {"edge_count", result.edge_count},
      {"limit_applied", result.limit_applied},
      {"nodes", std::move(nodes_arr)},
      {"edges", std::move(edges_arr)},
  };

  if (!result.missing_object_ids.empty()) {
    out["missing_object_ids"] = result.missing_object_ids;
  }
  if (!result.error_message.empty()) {
    out["error"] = result.error_message;
  }

  return out;
}

}  // namespace svp::query
