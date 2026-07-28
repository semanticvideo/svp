#include "query_output.hpp"

#include "svp/query/query_ops.hpp"
#include "svp/query/query_reader.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string_view>

namespace query_cmd {
namespace {

std::string compact_node_summary(const nlohmann::json& summary) {
  if (!summary.is_object()) return "";
  const auto kind = summary.value("kind", "");
  if (kind == "word") {
    return "word text=\"" + summary.value("text", "?") + "\""
           + " start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "speaker") {
    return "speaker name=\"" + summary.value("display_name", "?") + "\"";
  }
  if (kind == "speaker_segment") {
    return "segment speaker=" + summary.value("speaker_id", "?")
           + " start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "text_region") {
    return "text_region type=" + summary.value("observation_type", "?")
           + " start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "text_observation") {
    return "text_obs raw=\"" + summary.value("raw_text", "?") + "\""
           + " norm=\"" + summary.value("normalized_text", "?") + "\"";
  }
  if (kind == "numeric_value") {
    return "numeric_value value=" + summary.value("numeric_value", "?")
           + " unit=" + summary.value("unit", "?");
  }
  if (kind == "evidence_crop") {
    return "crop path=" + summary.value("crop_file_path", "?")
           + " size="
           + std::to_string(summary.value("crop_size_bytes", 0));
  }
  if (kind == "color_observation") {
    return "color dominant=" + summary.value("dominant_bucket", "?")
           + " target=" + summary.value("target_type", "?")
           + ":" + summary.value("target_id", "?");
  }
  if (kind == "frame") {
    return "frame pts_us=" + std::to_string(summary.value("pts_us", 0));
  }
  if (kind == "shot") {
    return "shot start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "scene") {
    return "scene start_us=" + std::to_string(summary.value("start_us", 0))
           + " end_us=" + std::to_string(summary.value("end_us", 0));
  }
  if (kind == "entity") {
    return "entity type=" + summary.value("entity_type", "?")
           + " label=" + summary.value("label", "?");
  }
  if (kind == "entity_track") {
    return "track entity=" + summary.value("entity_id", "?");
  }
  if (kind == "region") {
    return "region frame=" + summary.value("frame_id", "?");
  }
  return kind;
}

}  // namespace

void print_layers(const std::filesystem::path& package_path, bool json_output) {
  const auto summary = svp::query::list_layers(package_path);

  if (json_output) {
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& layer : summary.layers) {
      nlohmann::json entry = {
          {"section", layer.section},
          {"entry", layer.entry},
          {"kind", layer.kind},
          {"present", layer.present},
          {"record_count", layer.record_count},
      };
      if (layer.has_malformed) {
        entry["has_malformed"] = true;
        entry["malformed_line_count"] = layer.malformed_line_count;
        entry["error"] = layer.error_message;
      }
      layers.push_back(entry);
    }
    std::cout << nlohmann::json{
        {"total_entries", summary.total_entries},
        {"layers", layers},
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Package layers (" << summary.total_entries << " present)\n";
  for (const auto& layer : summary.layers) {
    std::cout << "  [" << layer.section << "] " << layer.entry;
    if (layer.present) {
      std::cout << " (" << layer.record_count << " records)";
    } else {
      std::cout << " (missing)";
    }
    if (layer.has_malformed) {
      std::cout << " MALFORMED";
      if (layer.malformed_line_count > 0) {
        std::cout << " (" << layer.malformed_line_count << " bad lines)";
      }
    }
    std::cout << "\n";
  }
}

void print_transcript(const std::filesystem::path& package_path,
                      bool json_output) {
  const auto result = svp::query::transcript_summary(package_path);

  if (!result.present) {
    if (json_output) {
      std::cout << R"({"present":false})" << "\n";
    } else {
      std::cout << "Transcript: not present\n";
    }
    return;
  }

  if (!result.parsed) {
    if (json_output) {
      std::cout << nlohmann::json{
          {"present", true},
          {"parsed", false},
          {"error", result.error_message},
      }.dump(2) << "\n";
    } else {
      std::cout << "Transcript: present but JSON parse failed: "
                << result.error_message << "\n";
    }
    return;
  }

  if (json_output) {
    nlohmann::json out = result.transcript_json;
    out["word_count_file"] = result.word_count_file;
    out["speaker_count_file"] = result.speaker_count_file;
    out["speech_region_count_file"] = result.speech_region_count;
    std::cout << out.dump(2) << "\n";
    return;
  }

  std::cout << "Transcript summary\n";
  const auto lang = result.transcript_json.value("language", nlohmann::json{});
  if (lang.is_object()) {
    std::cout << "  language: " << lang.value("primary", "unknown");
    std::cout << " (" << lang.value("mode", "unknown") << ")\n";
  } else {
    std::cout << "  language: not present\n";
  }
  std::cout << "  word_count (declared): "
            << result.transcript_json.value("word_count", 0) << "\n";
  std::cout << "  word_count (file): " << result.word_count_file << "\n";
  if (result.transcript_json.contains("speaker_count")) {
    std::cout << "  speaker_count (declared): "
              << result.transcript_json.value("speaker_count", 0) << "\n";
  }
  std::cout << "  speaker_count (file): " << result.speaker_count_file
            << "\n";
  std::cout << "  speech_regions (file): " << result.speech_region_count
            << "\n";
  if (result.transcript_json.contains("duration_us")) {
    std::cout << "  duration_us: "
              << result.transcript_json.value("duration_us", 0) << "\n";
  }

  if (result.word_count_file > 0) {
    const auto words =
        svp::query::read_jsonl_entry(package_path, "transcript/words.jsonl");
    if (words.readable && !words.records.empty()) {
      const auto& first = words.records.front();
      const auto& last = words.records.back();
      std::cout << "  word range: " << first.value("id", "?") << " to "
                << last.value("id", "?") << " ("
                << first.value("start_us", 0) << "us - "
                << last.value("end_us", 0) << "us)\n";
    }
  }
}

void print_find_words(const std::filesystem::path& package_path,
                      const std::string& search_text,
                      std::size_t max_results,
                      bool json_output) {
  const auto matches =
      svp::query::find_words(package_path, search_text, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& match : matches) {
      arr.push_back(match.record);
    }
    std::cout << nlohmann::json{
        {"search_text", search_text},
        {"match_count", matches.size()},
        {"matches", arr},
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Found " << matches.size() << " word(s) matching \""
            << search_text << "\"\n";
  for (const auto& match : matches) {
    std::cout << "  " << match.record.value("id", "?")
              << " text=\"" << match.record.value("text", "?") << "\""
              << " start_us=" << match.record.value("start_us", 0)
              << " end_us=" << match.record.value("end_us", 0)
              << " speaker=" << match.record.value("speaker_id", "?")
              << "\n";
  }
}

void print_speakers(const std::filesystem::path& package_path,
                    bool json_output) {
  const auto speakers = svp::query::list_speakers(package_path);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& speaker : speakers) {
      nlohmann::json record = speaker.record;
      record["word_count"] = speaker.word_count;
      arr.push_back(record);
    }
    std::cout << nlohmann::json{
        {"speaker_count", speakers.size()},
        {"speakers", arr},
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Speakers (" << speakers.size() << ")\n";
  for (const auto& speaker : speakers) {
    std::cout << "  " << speaker.record.value("id", "?")
              << " name=\""
              << speaker.record.value("display_name", "?") << "\""
              << " words=" << speaker.word_count
              << " speech_us="
              << speaker.record.value("total_speech_us", 0) << "\n";
  }
}

void print_ocr(const std::filesystem::path& package_path,
               const std::optional<std::string>& text_filter,
               std::size_t max_results,
               bool json_output) {
  const auto observations = svp::query::list_ocr_observations(
      package_path, text_filter, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& observation : observations) {
      arr.push_back(observation.record);
    }
    std::cout << nlohmann::json{
        {"observation_count", observations.size()},
        {"observations", arr},
    }.dump(2) << "\n";
    return;
  }

  std::cout << "OCR text observations (" << observations.size() << ")\n";
  for (const auto& observation : observations) {
    std::cout << "  "
              << observation.record.value("text_observation_id", "?")
              << " type="
              << observation.record.value("observation_type", "?")
              << " raw=\"" << observation.record.value("raw_text", "")
              << "\" confidence="
              << observation.record.value("confidence", 0.0);

    const auto crop_refs = observation.record.find("evidence_crop_refs");
    if (crop_refs != observation.record.end() && crop_refs->is_array() &&
        !crop_refs->empty()) {
      std::cout << " crops=[";
      bool first = true;
      for (const auto& ref : *crop_refs) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << ref.get<std::string>();
      }
      std::cout << "]";
    }
    std::cout << "\n";
  }

  if (!text_filter.has_value()) {
    const auto crops = svp::query::read_jsonl_entry(
        package_path, "text/evidence_crops.jsonl");
    if (crops.readable && !crops.records.empty()) {
      std::cout << "\nEvidence crops (" << crops.records.size() << ")\n";
      for (const auto& crop : crops.records) {
        std::cout << "  " << crop.value("crop_id", "?")
                  << " path=" << crop.value("crop_file_path", "?")
                  << " size=" << crop.value("crop_size_bytes", 0)
                  << " bytes\n";
      }
    }
  }
}

void print_colors(const std::filesystem::path& package_path,
                  const std::optional<std::string>& dominant_filter,
                  std::optional<double> min_coverage,
                  std::size_t max_results,
                  bool json_output) {
  const auto observations = svp::query::list_color_observations(
      package_path, dominant_filter, min_coverage, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& observation : observations) {
      arr.push_back(observation.record);
    }
    std::cout << nlohmann::json{
        {"observation_count", observations.size()},
        {"observations", arr},
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Color observations (" << observations.size() << ")\n";
  for (const auto& observation : observations) {
    std::cout << "  "
              << observation.record.value("color_observation_id", "?")
              << " target=" << observation.record.value("target_type", "?")
              << ":" << observation.record.value("target_id", "?")
              << " dominant="
              << observation.record.value("dominant_bucket", "?")
              << " coverage_total="
              << observation.record.value("coverage_total", 0.0);

    const auto buckets = observation.record.find("bucket_coverage");
    if (buckets != observation.record.end() && buckets->is_object()) {
      std::cout << " buckets={";
      bool first = true;
      for (auto it = buckets->begin(); it != buckets->end(); ++it) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << it.key() << "=" << it.value();
      }
      std::cout << "}";
    }
    std::cout << "\n";
  }
}

void print_validation(const std::filesystem::path& package_path,
                      bool json_output) {
  const auto info = svp::query::show_validation(package_path);

  if (!info.present) {
    if (json_output) {
      std::cout << R"({"present":false})" << "\n";
    } else {
      std::cout << "Validation: not present\n";
    }
    return;
  }

  if (!info.parsed) {
    if (json_output) {
      std::cout << nlohmann::json{
          {"present", true},
          {"parsed", false},
          {"error", info.error_message},
      }.dump(2) << "\n";
    } else {
      std::cout << "Validation: present but JSON parse failed: "
                << info.error_message << "\n";
    }
    return;
  }

  if (json_output) {
    nlohmann::json out = {{"present", true}, {"parsed", true}};
    out["record"] = info.record;
    std::cout << out.dump(2) << "\n";
    return;
  }

  std::cout << "Validation status\n";
  std::cout << "  status: " << info.record.value("status", "unknown")
            << "\n";
  if (info.record.contains("core_status")) {
    std::cout << "  core_status: "
              << info.record.value("core_status", "unknown") << "\n";
  }
  if (info.record.contains("authenticity_status")) {
    std::cout << "  authenticity_status: "
              << info.record.value("authenticity_status", "unknown") << "\n";
  }
}

void print_relationships(
    const std::filesystem::path& package_path,
    const std::optional<std::string>& class_filter,
    std::size_t max_results,
    bool json_output) {
  const auto summary = svp::query::relationship_summary(package_path);

  if (!summary.present) {
    if (json_output) {
      std::cout << R"({"present":false})" << "\n";
    } else {
      std::cout << "Relationships: not present\n";
    }
    return;
  }

  if (!summary.readable) {
    if (json_output) {
      std::cout << nlohmann::json{
          {"present", true},
          {"readable", false},
          {"error", summary.error_message},
      }.dump(2) << "\n";
    } else {
      std::cout << "Relationships: present but unreadable: "
                << summary.error_message << "\n";
    }
    return;
  }

  const auto relationships = svp::query::list_relationships(
      package_path, class_filter, max_results);

  if (json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& relationship : relationships) {
      nlohmann::json entry = relationship.record;
      entry["_relationship_class"] = relationship.relationship_class;
      arr.push_back(entry);
    }
    std::cout << nlohmann::json{
        {"present", true},
        {"readable", true},
        {"total_count", summary.total_count},
        {"support_count", summary.support_count},
        {"semantic_count", summary.semantic_count},
        {"unknown_count", summary.unknown_count},
        {"returned_count", relationships.size()},
        {"relationships", arr},
    }.dump(2) << "\n";
    return;
  }

  std::cout << "Relationships (" << summary.total_count << " total)\n";
  std::cout << "  support: " << summary.support_count << "\n";
  std::cout << "  semantic: " << summary.semantic_count << "\n";
  std::cout << "  unknown: " << summary.unknown_count << "\n";
  std::cout << "  showing " << relationships.size() << " (limit "
            << max_results << ")\n\n";

  for (const auto& relationship : relationships) {
    const auto& record = relationship.record;
    std::cout << "  " << record.value("id", "?") << " ["
              << relationship.relationship_class << "] "
              << record.value("type", "?") << "  "
              << record.value("source_id", "?") << " -> "
              << record.value("target_id", "?");
    if (record.contains("confidence")) {
      std::cout << "  conf=" << record.value("confidence", 0.0);
    }
    std::cout << "\n";
  }
}

void print_traversal(const std::filesystem::path& package_path,
                     const svp::query::TraversalOptions& options,
                     bool json_output) {
  auto result = svp::query::traverse_relationships(package_path, options);

  if (json_output) {
    std::cout << svp::query::traversal_result_to_json(result).dump(2) << "\n";
    return;
  }

  if (!result.error_message.empty()) {
    std::cout << "Traversal error: " << result.error_message << "\n";
    return;
  }

  std::cout << "Traversal from " << result.start_id << " (depth "
            << result.requested_depth << ")\n";
  std::cout << "  nodes visited: " << result.visited_node_count << "\n";
  std::cout << "  edges found: " << result.edge_count << "\n";
  if (result.limit_applied) {
    std::cout << "  (limit applied)\n";
  }
  if (!result.time_window_json.is_null()) {
    std::cout << "  time window: " << result.time_window_json.dump() << "\n";
  }
  if (result.graph_health.total_edges > 0) {
    std::cout << "  graph health: " << result.graph_health.total_edges
              << " edges, " << result.graph_health.total_nodes << " nodes";
    if (result.graph_health.unresolved_endpoints > 0) {
      std::cout << ", " << result.graph_health.unresolved_endpoints
                << " unresolved";
    }
    if (result.graph_health.unknown_relationship_types > 0) {
      std::cout << ", " << result.graph_health.unknown_relationship_types
                << " unknown types";
    }
    std::cout << "\n";
  }
  if (!result.missing_object_ids.empty()) {
    std::cout << "  unresolved IDs: " << result.missing_object_ids.size()
              << "\n";
    for (const auto& id : result.missing_object_ids) {
      std::cout << "    - " << id << "\n";
    }
  }
  std::cout << "\nNodes:\n";
  for (const auto& node : result.nodes) {
    std::cout << "  [d" << node.depth << "] " << node.object_id;
    if (node.resolved) {
      const auto description = compact_node_summary(node.summary);
      if (!description.empty()) {
        std::cout << "  " << description;
      }
    } else {
      std::cout << "  (unresolved)";
    }
    std::cout << "\n";
  }

  std::cout << "\nEdges:\n";
  for (const auto& edge : result.edges) {
    std::cout << "  [d" << edge.depth << "] " << edge.direction << " "
              << edge.source_id << " -> " << edge.target_id << "  ["
              << edge.relationship_class << "] " << edge.relationship_type;
    if (!edge.relationship_id.empty()) {
      std::cout << "  id=" << edge.relationship_id;
    }
    std::cout << "\n";
  }
}

void print_path(const std::filesystem::path& package_path,
                const svp::query::TraversalOptions& options,
                bool json_output) {
  auto result = svp::query::find_shortest_path(package_path, options);

  if (json_output) {
    std::cout << svp::query::path_result_to_json(result).dump(2) << "\n";
    return;
  }

  if (!result.error_message.empty() && !result.path_found) {
    std::cout << "Path error: " << result.error_message << "\n";
    return;
  }

  std::cout << "Shortest path from " << result.start_id << " to "
            << result.target_id << "\n";
  std::cout << "  path found: " << (result.path_found ? "yes" : "no")
            << "\n";
  std::cout << "  path length: " << result.path_edges.size()
            << " edges\n\n";

  std::cout << "Nodes:\n";
  for (const auto& node : result.path_nodes) {
    std::cout << "  [d" << node.depth << "] " << node.object_id;
    if (node.resolved) {
      const auto description = compact_node_summary(node.summary);
      if (!description.empty()) {
        std::cout << "  " << description;
      }
    } else {
      std::cout << "  (unresolved)";
    }
    std::cout << "\n";
  }

  std::cout << "\nEdges:\n";
  for (const auto& edge : result.path_edges) {
    std::cout << "  [d" << edge.depth << "] " << edge.direction << " "
              << edge.source_id << " -> " << edge.target_id << "  ["
              << edge.relationship_class << "] " << edge.relationship_type;
    if (!edge.relationship_id.empty()) {
      std::cout << "  id=" << edge.relationship_id;
    }
    std::cout << "\n";
  }
}

void print_context_result(const svp::query::ContextResult& result) {
  if (!result.error_message.empty()) {
    std::cout << "Context error: " << result.error_message << "\n";
    return;
  }

  std::cout << "Context for " << result.object_id << "\n";
  std::cout << "  resolved: " << (result.resolved ? "yes" : "no") << "\n";
  if (!result.source_layer.empty()) {
    std::cout << "  source_layer: " << result.source_layer << "\n";
  }
  const auto description = compact_node_summary(result.summary);
  if (!description.empty()) {
    std::cout << "  summary: " << description << "\n";
  }
  std::cout << "  neighbors: " << result.context_nodes.size() << "\n\n";

  std::cout << "Context nodes:\n";
  for (const auto& node : result.context_nodes) {
    std::cout << "  [d" << node.depth << "] " << node.object_id;
    if (node.resolved) {
      const auto node_description = compact_node_summary(node.summary);
      if (!node_description.empty()) {
        std::cout << "  " << node_description;
      }
    } else {
      std::cout << "  (unresolved)";
    }
    std::cout << "\n";
  }

  std::cout << "\nContext edges:\n";
  for (const auto& edge : result.context_edges) {
    std::cout << "  [d" << edge.depth << "] " << edge.direction << " "
              << edge.source_id << " -> " << edge.target_id << "  ["
              << edge.relationship_class << "] " << edge.relationship_type;
    if (!edge.relationship_id.empty()) {
      std::cout << "  id=" << edge.relationship_id;
    }
    std::cout << "\n";
  }
}

}  // namespace query_cmd
