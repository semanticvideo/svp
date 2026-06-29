#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace svp::query {

enum class TraversalDirection {
  Outgoing,
  Incoming,
  Both,
};

struct TimeWindow {
  std::optional<std::int64_t> at_us;
  std::optional<std::int64_t> start_us;
  std::optional<std::int64_t> end_us;

  [[nodiscard]] bool active() const {
    return at_us.has_value() || start_us.has_value() || end_us.has_value();
  }

  [[nodiscard]] bool matches(std::int64_t rel_start, std::int64_t rel_end) const;
};

struct TraversalOptions {
  std::string start_id;
  std::string target_id;
  int max_depth = 2;
  TraversalDirection direction = TraversalDirection::Both;
  std::string class_filter = "all";
  std::optional<std::string> type_filter;
  std::size_t limit = 1000;
  TimeWindow time_window;
  bool context_mode = false;
};

struct CatalogEntry {
  std::string object_id;
  std::string source_layer;
  nlohmann::json record;
};

struct ObjectCatalog {
  std::unordered_map<std::string, CatalogEntry> entries;

  [[nodiscard]] const CatalogEntry* find(const std::string& object_id) const;

  [[nodiscard]] nlohmann::json node_summary(const std::string& object_id) const;
};

struct TraversalEdge {
  std::string relationship_id;
  std::string relationship_type;
  std::string relationship_class;
  std::string source_id;
  std::string target_id;
  std::string direction;
  int depth = 0;
  nlohmann::json record;
};

struct TraversalNode {
  std::string object_id;
  int depth = 0;
  bool resolved = false;
  std::string source_layer;
  nlohmann::json summary;
};

struct GraphHealthReport {
  std::size_t total_edges = 0;
  std::size_t total_nodes = 0;
  std::size_t unresolved_endpoints = 0;
  std::size_t unknown_relationship_types = 0;
  std::unordered_map<std::string, std::size_t> class_counts;
  std::unordered_map<std::string, std::size_t> type_counts;
  std::vector<std::string> unresolved_ids;
  std::vector<std::string> unknown_types;
};

struct TraversalResult {
  std::string start_id;
  int requested_depth = 0;
  std::size_t visited_node_count = 0;
  std::size_t edge_count = 0;
  bool limit_applied = false;
  std::vector<TraversalNode> nodes;
  std::vector<TraversalEdge> edges;
  std::vector<std::string> missing_object_ids;
  std::string error_message;
  GraphHealthReport graph_health;
  nlohmann::json time_window_json;
};

struct PathResult {
  std::string start_id;
  std::string target_id;
  bool path_found = false;
  std::vector<TraversalNode> path_nodes;
  std::vector<TraversalEdge> path_edges;
  std::string error_message;
};

struct ContextResult {
  std::string object_id;
  bool resolved = false;
  std::string source_layer;
  nlohmann::json summary;
  std::vector<TraversalNode> context_nodes;
  std::vector<TraversalEdge> context_edges;
  std::string error_message;
};

[[nodiscard]] ObjectCatalog build_object_catalog(
    const std::filesystem::path& package_path);

[[nodiscard]] TraversalResult traverse_relationships(
    const std::filesystem::path& package_path,
    const TraversalOptions& options);

[[nodiscard]] nlohmann::json traversal_result_to_json(const TraversalResult& result);

[[nodiscard]] PathResult find_shortest_path(
    const std::filesystem::path& package_path,
    const TraversalOptions& options);

[[nodiscard]] nlohmann::json path_result_to_json(const PathResult& result);

[[nodiscard]] ContextResult build_context(
    const std::filesystem::path& package_path,
    const std::string& object_id,
    std::size_t limit);

[[nodiscard]] nlohmann::json context_result_to_json(const ContextResult& result);

[[nodiscard]] nlohmann::json time_window_to_json(const TimeWindow& tw);

}  // namespace svp::query
