#include "svp/query/traversal.hpp"

#include "svp/query/query_reader.hpp"
#include "svp/package/relationship_type_policy.hpp"

#include <algorithm>
#include <array>
#include <climits>
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
    {"embeddings/embeddings.index.jsonl",   {"id", "embedding_id", "embedding_set_id"}},
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

nlohmann::json make_mask_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "mask";
  if (record.contains("block_path")) summary["block_path"] = record["block_path"];
  if (record.contains("block_size_bytes")) summary["block_size_bytes"] = record["block_size_bytes"];
  if (record.contains("frame_id")) summary["frame_id"] = record["frame_id"];
  return summary;
}

nlohmann::json make_depth_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "depth";
  if (record.contains("block_path")) summary["block_path"] = record["block_path"];
  if (record.contains("block_size_bytes")) summary["block_size_bytes"] = record["block_size_bytes"];
  if (record.contains("frame_id")) summary["frame_id"] = record["frame_id"];
  return summary;
}

nlohmann::json make_embedding_summary(const nlohmann::json& record) {
  nlohmann::json summary;
  summary["kind"] = "embedding";
  if (record.contains("embedding_set_id")) summary["embedding_set_id"] = record["embedding_set_id"];
  if (record.contains("source_type")) summary["source_type"] = record["source_type"];
  if (record.contains("source_id")) summary["source_id"] = record["source_id"];
  if (record.contains("block_path")) summary["block_path"] = record["block_path"];
  if (record.contains("block_size_bytes")) summary["block_size_bytes"] = record["block_size_bytes"];
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
  if (source_layer == "spatial/masks.index.jsonl") return make_mask_summary(record);
  if (source_layer == "spatial/depth.index.jsonl") return make_depth_summary(record);
  if (source_layer == "embeddings/embeddings.index.jsonl") return make_embedding_summary(record);
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

std::int64_t json_int_val(const nlohmann::json& j, std::string_view key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number_integer()) return 0;
  return it->get<std::int64_t>();
}

}  // namespace

bool TimeWindow::matches(std::int64_t rel_start, std::int64_t rel_end) const {
  if (!active()) return true;

  if (at_us.has_value()) {
    const auto t = *at_us;
    if (rel_start == rel_end) {
      return rel_start == t;
    }
    return t >= rel_start && t < rel_end;
  }

  const auto win_start = start_us.value_or(0);
  const auto win_end = end_us.value_or(INT64_MAX);

  if (rel_start == rel_end) {
    return rel_start >= win_start && rel_start < win_end;
  }
  return rel_start < win_end && win_start < rel_end;
}

nlohmann::json time_window_to_json(const TimeWindow& tw) {
  nlohmann::json j;
  if (tw.at_us.has_value()) j["at_us"] = *tw.at_us;
  if (tw.start_us.has_value()) j["start_us"] = *tw.start_us;
  if (tw.end_us.has_value()) j["end_us"] = *tw.end_us;
  return j;
}

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
  result.time_window_json = time_window_to_json(options.time_window);

  if (options.start_id.empty()) {
    result.error_message = "start_id is required for traversal";
    return result;
  }

  if (options.time_window.at_us.has_value() &&
      (options.time_window.start_us.has_value() ||
       options.time_window.end_us.has_value())) {
    result.error_message = "cannot specify both --at-us and --start-us/--end-us";
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

    const auto rel_start = json_int_val(record, "start_us");
    const auto rel_end = json_int_val(record, "end_us");
    if (options.time_window.active() && !options.time_window.matches(rel_start, rel_end)) {
      continue;
    }

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

  ObjectCatalog catalog = build_object_catalog(package_path);

  std::unordered_set<std::string> visited;
  std::unordered_set<std::string> emitted_edge_ids;
  std::queue<std::pair<std::string, int>> frontier;
  frontier.push({options.start_id, 0});
  visited.insert(options.start_id);

  TraversalNode start_node;
  start_node.object_id = options.start_id;
  start_node.depth = 0;
  start_node.resolved = catalog.find(options.start_id) != nullptr;
  const auto* start_entry = catalog.find(options.start_id);
  if (start_entry) start_node.source_layer = start_entry->source_layer;
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

        if (!edge.relationship_id.empty()) {
          if (emitted_edge_ids.count(edge.relationship_id) > 0) continue;
          emitted_edge_ids.insert(edge.relationship_id);
        }

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
          const auto* neighbor_entry = catalog.find(neighbor_id);
          if (neighbor_entry) tnode.source_layer = neighbor_entry->source_layer;
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

  auto& health = result.graph_health;
  health.total_edges = edges.size();
  health.total_nodes = nodes.size();
  for (const auto& edge : edges) {
    health.class_counts[edge.relationship_class]++;
    health.type_counts[edge.relationship_type]++;
    if (edge.relationship_class == "unknown") {
      health.unknown_types.push_back(edge.relationship_type);
    }
    if (catalog.find(edge.source_id) == nullptr) {
      health.unresolved_ids.push_back(edge.source_id);
    }
    if (catalog.find(edge.target_id) == nullptr) {
      health.unresolved_ids.push_back(edge.target_id);
    }
  }
  health.unresolved_endpoints = health.unresolved_ids.size();
  health.unknown_relationship_types = health.unknown_types.size();

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
    if (!node.source_layer.empty()) {
      n["source_layer"] = node.source_layer;
    }
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

  nlohmann::json health_json = {
      {"total_edges", result.graph_health.total_edges},
      {"total_nodes", result.graph_health.total_nodes},
      {"unresolved_endpoints", result.graph_health.unresolved_endpoints},
      {"unknown_relationship_types", result.graph_health.unknown_relationship_types},
  };
  nlohmann::json class_counts_json = nlohmann::json::object();
  for (const auto& [cls, cnt] : result.graph_health.class_counts) {
    class_counts_json[cls] = cnt;
  }
  health_json["class_counts"] = class_counts_json;

  nlohmann::json type_counts_json = nlohmann::json::object();
  for (const auto& [type, cnt] : result.graph_health.type_counts) {
    type_counts_json[type] = cnt;
  }
  health_json["type_counts"] = type_counts_json;

  if (!result.graph_health.unresolved_ids.empty()) {
    health_json["unresolved_ids"] = result.graph_health.unresolved_ids;
  }
  if (!result.graph_health.unknown_types.empty()) {
    health_json["unknown_types"] = result.graph_health.unknown_types;
  }

  nlohmann::json out = {
      {"start_id", result.start_id},
      {"requested_depth", result.requested_depth},
      {"visited_node_count", result.visited_node_count},
      {"edge_count", result.edge_count},
      {"limit_applied", result.limit_applied},
      {"nodes", std::move(nodes_arr)},
      {"edges", std::move(edges_arr)},
      {"graph_health", std::move(health_json)},
  };

  if (!result.time_window_json.is_null()) {
    out["time_window"] = result.time_window_json;
  }
  if (!result.missing_object_ids.empty()) {
    out["missing_object_ids"] = result.missing_object_ids;
  }
  if (!result.error_message.empty()) {
    out["error"] = result.error_message;
  }

  return out;
}

PathResult find_shortest_path(
    const std::filesystem::path& package_path,
    const TraversalOptions& options) {
  PathResult result;
  result.start_id = options.start_id;
  result.target_id = options.target_id;

  if (options.start_id.empty() || options.target_id.empty()) {
    result.error_message = "both --from and --to are required for path finding";
    return result;
  }

  if (options.start_id == options.target_id) {
    result.path_found = true;
    ObjectCatalog catalog = build_object_catalog(package_path);
    TraversalNode node;
    node.object_id = options.start_id;
    node.depth = 0;
    node.resolved = catalog.find(options.start_id) != nullptr;
    const auto* entry = catalog.find(options.start_id);
    if (entry) node.source_layer = entry->source_layer;
    node.summary = catalog.node_summary(options.start_id);
    result.path_nodes.push_back(std::move(node));
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

    const auto rel_start = json_int_val(record, "start_us");
    const auto rel_end = json_int_val(record, "end_us");
    if (options.time_window.active() && !options.time_window.matches(rel_start, rel_end)) {
      continue;
    }

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

  ObjectCatalog catalog = build_object_catalog(package_path);

  std::unordered_map<std::string, int> depth_map;
  std::unordered_map<std::string, std::string> parent_map;
  std::unordered_map<std::string, std::size_t> parent_edge_idx;
  std::queue<std::string> frontier;

  frontier.push(options.start_id);
  depth_map[options.start_id] = 0;

  bool found = false;

  while (!frontier.empty() && !found) {
    const auto current_id = frontier.front();
    frontier.pop();

    const auto current_depth = depth_map[current_id];
    if (current_depth >= options.max_depth) continue;

    const auto node_it = nodes.find(current_id);
    if (node_it == nodes.end()) continue;

    const auto process_neighbors = [&](const std::vector<std::size_t>& edge_indices,
                                        const std::string& direction) {
      for (const auto idx : edge_indices) {
        if (found) break;
        const auto& edge = edges[idx];
        const auto neighbor_id = (direction == "outgoing")
            ? edge.target_id : edge.source_id;

        if (depth_map.count(neighbor_id) > 0) continue;

        depth_map[neighbor_id] = current_depth + 1;
        parent_map[neighbor_id] = current_id;
        parent_edge_idx[neighbor_id] = idx;

        if (neighbor_id == options.target_id) {
          found = true;
          break;
        }

        frontier.push(neighbor_id);
      }
    };

    if (options.direction == TraversalDirection::Outgoing ||
        options.direction == TraversalDirection::Both) {
      process_neighbors(node_it->second.outgoing, "outgoing");
    }
    if (options.direction == TraversalDirection::Incoming ||
        options.direction == TraversalDirection::Both) {
      process_neighbors(node_it->second.incoming, "incoming");
    }
  }

  if (!found) {
    result.error_message = "no path found from " + options.start_id +
                           " to " + options.target_id +
                           " within depth " + std::to_string(options.max_depth);
    return result;
  }

  result.path_found = true;

  std::vector<std::string> path_ids;
  std::vector<std::size_t> path_edge_indices;
  std::string current = options.target_id;
  while (current != options.start_id) {
    path_ids.push_back(current);
    path_edge_indices.push_back(parent_edge_idx[current]);
    current = parent_map[current];
  }
  path_ids.push_back(options.start_id);
  std::reverse(path_ids.begin(), path_ids.end());
  std::reverse(path_edge_indices.begin(), path_edge_indices.end());

  for (std::size_t i = 0; i < path_ids.size(); ++i) {
    TraversalNode node;
    node.object_id = path_ids[i];
    node.depth = static_cast<int>(i);
    node.resolved = catalog.find(path_ids[i]) != nullptr;
    const auto* entry = catalog.find(path_ids[i]);
    if (entry) node.source_layer = entry->source_layer;
    node.summary = catalog.node_summary(path_ids[i]);
    result.path_nodes.push_back(std::move(node));
  }

  for (std::size_t i = 0; i < path_edge_indices.size(); ++i) {
    const auto& edge = edges[path_edge_indices[i]];
    TraversalEdge tedge;
    tedge.relationship_id = edge.relationship_id;
    tedge.relationship_type = edge.relationship_type;
    tedge.relationship_class = edge.relationship_class;
    tedge.source_id = edge.source_id;
    tedge.target_id = edge.target_id;
    tedge.direction = (edge.source_id == path_ids[i]) ? "outgoing" : "incoming";
    tedge.depth = static_cast<int>(i + 1);
    tedge.record = edge.record;
    result.path_edges.push_back(std::move(tedge));
  }

  return result;
}

nlohmann::json path_result_to_json(const PathResult& result) {
  nlohmann::json nodes_arr = nlohmann::json::array();
  for (const auto& node : result.path_nodes) {
    nlohmann::json n = {
        {"object_id", node.object_id},
        {"depth", node.depth},
        {"resolved", node.resolved},
    };
    if (!node.source_layer.empty()) n["source_layer"] = node.source_layer;
    if (!node.summary.is_null()) n["summary"] = node.summary;
    nodes_arr.push_back(std::move(n));
  }

  nlohmann::json edges_arr = nlohmann::json::array();
  for (const auto& edge : result.path_edges) {
    edges_arr.push_back({
        {"relationship_id", edge.relationship_id},
        {"relationship_type", edge.relationship_type},
        {"relationship_class", edge.relationship_class},
        {"source_id", edge.source_id},
        {"target_id", edge.target_id},
        {"direction", edge.direction},
        {"depth", edge.depth},
    });
  }

  nlohmann::json j = {
      {"start_id", result.start_id},
      {"target_id", result.target_id},
      {"path_found", result.path_found},
      {"path_length", result.path_edges.size()},
      {"nodes", std::move(nodes_arr)},
      {"edges", std::move(edges_arr)},
  };

  if (!result.error_message.empty()) {
    j["error"] = result.error_message;
  }

  return j;
}

ContextResult build_context(
    const std::filesystem::path& package_path,
    const std::string& object_id,
    std::size_t limit) {
  ContextResult result;
  result.object_id = object_id;

  if (object_id.empty()) {
    result.error_message = "object_id is required for context";
    return result;
  }

  ObjectCatalog catalog = build_object_catalog(package_path);

  const auto* entry = catalog.find(object_id);
  if (entry) {
    result.resolved = true;
    result.source_layer = entry->source_layer;
    result.summary = catalog.node_summary(object_id);
  }

  const auto jsonl = read_jsonl_entry(package_path, "relationships/relationships.jsonl");
  if (!jsonl.present || !jsonl.readable) {
    if (!result.resolved) {
      result.error_message = "object not found in catalog and relationships not readable";
    }
    return result;
  }

  std::unordered_set<std::string> emitted_edge_ids;
  std::size_t edge_count = 0;

  for (const auto& record : jsonl.records) {
    if (edge_count >= limit) break;

    const auto source_id = json_string_val(record, "source_id");
    const auto target_id = json_string_val(record, "target_id");
    const auto rel_id = json_string_val(record, "id");
    const auto type = json_string_val(record, "type");

    if (source_id != object_id && target_id != object_id) continue;

    if (!rel_id.empty()) {
      if (emitted_edge_ids.count(rel_id) > 0) continue;
      emitted_edge_ids.insert(rel_id);
    }

    const auto cls = svp::package::classify_relationship_type(type);
    const auto class_str = std::string{svp::package::relationship_class_to_string(cls)};

    const auto neighbor_id = (source_id == object_id) ? target_id : source_id;
    const auto direction = (source_id == object_id) ? "outgoing" : "incoming";

    TraversalEdge tedge;
    tedge.relationship_id = rel_id;
    tedge.relationship_type = type;
    tedge.relationship_class = class_str;
    tedge.source_id = source_id;
    tedge.target_id = target_id;
    tedge.direction = direction;
    tedge.depth = 1;
    tedge.record = record;
    result.context_edges.push_back(std::move(tedge));

    TraversalNode tnode;
    tnode.object_id = neighbor_id;
    tnode.depth = 1;
    tnode.resolved = catalog.find(neighbor_id) != nullptr;
    const auto* neighbor_entry = catalog.find(neighbor_id);
    if (neighbor_entry) tnode.source_layer = neighbor_entry->source_layer;
    tnode.summary = catalog.node_summary(neighbor_id);
    result.context_nodes.push_back(std::move(tnode));

    ++edge_count;
  }

  return result;
}

ContextResult build_context(
    const std::filesystem::path& package_path,
    const ContextOptions& options) {
  ContextResult result;
  result.object_id = options.object_id;

  if (options.object_id.empty()) {
    result.error_message = "object_id is required for context";
    return result;
  }

  if (options.at_us.has_value() &&
      (options.start_us.has_value() || options.end_us.has_value())) {
    result.error_message = "cannot specify both at_us and start_us/end_us";
    return result;
  }

  ObjectCatalog catalog = build_object_catalog(package_path);

  const auto* entry = catalog.find(options.object_id);
  if (entry) {
    result.resolved = true;
    result.source_layer = entry->source_layer;
    result.summary = catalog.node_summary(options.object_id);
  }

  const auto jsonl = read_jsonl_entry(package_path, "relationships/relationships.jsonl");
  if (!jsonl.present || !jsonl.readable) {
    if (!result.resolved) {
      result.error_message = "object not found in catalog and relationships not readable";
    }
    return result;
  }

  // Build the graph for multi-hop traversal.
  std::vector<GraphEdge> edges;
  std::unordered_map<std::string, GraphNode> nodes;

  TimeWindow time_window;
  time_window.at_us = options.at_us;
  time_window.start_us = options.start_us;
  time_window.end_us = options.end_us;

  for (const auto& record : jsonl.records) {
    const auto type = json_string_val(record, "type");
    const auto source_id = json_string_val(record, "source_id");
    const auto target_id = json_string_val(record, "target_id");
    const auto rel_id = json_string_val(record, "id");

    if (source_id.empty() || target_id.empty()) continue;

    const auto cls = svp::package::classify_relationship_type(type);
    const auto class_str = std::string{svp::package::relationship_class_to_string(cls)};

    if (!class_matches(class_str, options.class_filter)) continue;

    const auto rel_start = json_int_val(record, "start_us");
    const auto rel_end = json_int_val(record, "end_us");
    if (time_window.active() && !time_window.matches(rel_start, rel_end)) {
      continue;
    }

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

  // BFS from the target object up to max_depth.
  std::unordered_set<std::string> visited;
  std::unordered_set<std::string> emitted_edge_ids;
  std::queue<std::pair<std::string, int>> frontier;
  frontier.push({options.object_id, 0});
  visited.insert(options.object_id);

  std::size_t edge_count = 0;
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
        if (edge_count >= options.limit) {
          limit_hit = true;
          break;
        }

        const auto& edge = edges[idx];

        if (!edge.relationship_id.empty()) {
          if (emitted_edge_ids.count(edge.relationship_id) > 0) continue;
          emitted_edge_ids.insert(edge.relationship_id);
        }

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
        result.context_edges.push_back(std::move(tedge));

        ++edge_count;

        if (visited.find(neighbor_id) == visited.end()) {
          visited.insert(neighbor_id);

          TraversalNode tnode;
          tnode.object_id = neighbor_id;
          tnode.depth = depth + 1;
          tnode.resolved = catalog.find(neighbor_id) != nullptr;
          const auto* neighbor_entry = catalog.find(neighbor_id);
          if (neighbor_entry) tnode.source_layer = neighbor_entry->source_layer;
          tnode.summary = catalog.node_summary(neighbor_id);
          result.context_nodes.push_back(std::move(tnode));

          frontier.push({neighbor_id, depth + 1});
        }
      }
    };

    collect_edges(node_it->second.outgoing, "outgoing");
    collect_edges(node_it->second.incoming, "incoming");
  }

  return result;
}

nlohmann::json context_result_to_json(const ContextResult& result) {
  nlohmann::json j = {
      {"object_id", result.object_id},
      {"resolved", result.resolved},
  };

  if (!result.source_layer.empty()) j["source_layer"] = result.source_layer;
  if (!result.summary.is_null()) j["summary"] = result.summary;

  nlohmann::json nodes_arr = nlohmann::json::array();
  for (const auto& node : result.context_nodes) {
    nlohmann::json n = {
        {"object_id", node.object_id},
        {"resolved", node.resolved},
    };
    if (!node.source_layer.empty()) n["source_layer"] = node.source_layer;
    if (!node.summary.is_null()) n["summary"] = node.summary;
    nodes_arr.push_back(std::move(n));
  }
  j["context_nodes"] = std::move(nodes_arr);

  nlohmann::json edges_arr = nlohmann::json::array();
  for (const auto& edge : result.context_edges) {
    edges_arr.push_back({
        {"relationship_id", edge.relationship_id},
        {"relationship_type", edge.relationship_type},
        {"relationship_class", edge.relationship_class},
        {"source_id", edge.source_id},
        {"target_id", edge.target_id},
        {"direction", edge.direction},
    });
  }
  j["context_edges"] = std::move(edges_arr);

  if (!result.error_message.empty()) {
    j["error"] = result.error_message;
  }

  return j;
}

GraphHealthDiagnostics compute_graph_health(
    const std::filesystem::path& package_path) {
  GraphHealthDiagnostics health;

  const auto jsonl = read_jsonl_entry(package_path, "relationships/relationships.jsonl");
  if (!jsonl.present) {
    health.error_message = "relationships file not present";
    return health;
  }
  if (!jsonl.readable) {
    health.error_message = jsonl.error_message;
    return health;
  }

  ObjectCatalog catalog = build_object_catalog(package_path);

  std::unordered_set<std::string> graph_node_ids;
  std::unordered_set<std::string> catalog_node_ids;
  for (const auto& [id, _] : catalog.entries) {
    catalog_node_ids.insert(id);
  }

  for (const auto& record : jsonl.records) {
    const auto type = json_string_val(record, "type");
    const auto source_id = json_string_val(record, "source_id");
    const auto target_id = json_string_val(record, "target_id");

    if (source_id.empty() || target_id.empty()) continue;

    ++health.total_edges;
    graph_node_ids.insert(source_id);
    graph_node_ids.insert(target_id);

    const auto cls = svp::package::classify_relationship_type(type);
    const auto class_str = std::string{svp::package::relationship_class_to_string(cls)};
    health.class_counts[class_str]++;
    health.type_counts[type]++;

    if (class_str == "unknown") {
      health.unknown_types.push_back(type);
    }

    if (catalog.find(source_id) == nullptr) {
      health.unresolved_ids.push_back(source_id);
    }
    if (catalog.find(target_id) == nullptr) {
      health.unresolved_ids.push_back(target_id);
    }
  }

  health.total_nodes = graph_node_ids.size();

  for (const auto& id : graph_node_ids) {
    if (catalog.find(id) != nullptr) {
      ++health.resolved_nodes;
    } else {
      ++health.unresolved_nodes;
    }
  }

  // Orphan nodes: in catalog but not in any relationship.
  for (const auto& id : catalog_node_ids) {
    if (graph_node_ids.count(id) == 0) {
      health.orphan_ids.push_back(id);
    }
  }
  health.orphan_nodes = health.orphan_ids.size();
  health.unknown_relationship_types = health.unknown_types.size();

  return health;
}

nlohmann::json graph_health_to_json(const GraphHealthDiagnostics& health) {
  nlohmann::json j = {
      {"total_edges", health.total_edges},
      {"total_nodes", health.total_nodes},
      {"resolved_nodes", health.resolved_nodes},
      {"unresolved_nodes", health.unresolved_nodes},
      {"orphan_nodes", health.orphan_nodes},
      {"unknown_relationship_types", health.unknown_relationship_types},
  };

  nlohmann::json class_counts_json = nlohmann::json::object();
  for (const auto& [cls, cnt] : health.class_counts) {
    class_counts_json[cls] = cnt;
  }
  j["class_counts"] = class_counts_json;

  nlohmann::json type_counts_json = nlohmann::json::object();
  for (const auto& [type, cnt] : health.type_counts) {
    type_counts_json[type] = cnt;
  }
  j["type_counts"] = type_counts_json;

  if (!health.unresolved_ids.empty()) {
    j["unresolved_ids"] = health.unresolved_ids;
  }
  if (!health.unknown_types.empty()) {
    j["unknown_types"] = health.unknown_types;
  }
  if (!health.orphan_ids.empty()) {
    j["orphan_ids"] = health.orphan_ids;
  }
  if (!health.error_message.empty()) {
    j["error"] = health.error_message;
  }

  return j;
}

}  // namespace svp::query
