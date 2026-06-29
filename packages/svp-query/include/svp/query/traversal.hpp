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

struct TraversalOptions {
  std::string start_id;
  int max_depth = 2;
  TraversalDirection direction = TraversalDirection::Both;
  std::string class_filter = "all";
  std::optional<std::string> type_filter;
  std::size_t limit = 1000;
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
  nlohmann::json summary;
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
};

[[nodiscard]] ObjectCatalog build_object_catalog(
    const std::filesystem::path& package_path);

[[nodiscard]] TraversalResult traverse_relationships(
    const std::filesystem::path& package_path,
    const TraversalOptions& options);

[[nodiscard]] nlohmann::json traversal_result_to_json(const TraversalResult& result);

}  // namespace svp::query
