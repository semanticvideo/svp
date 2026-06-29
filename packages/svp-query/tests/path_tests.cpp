#include "query_test_registry.hpp"
#include "fixtures/traversal_package_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>

void test_shortest_path_found() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "speaker_0001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);

  assert(result.path_found);
  assert(!result.path_nodes.empty());
  assert(result.path_nodes.front().object_id == "word_000001");
  assert(result.path_nodes.back().object_id == "speaker_0001");
  assert(result.path_edges.size() == result.path_nodes.size() - 1);

  std::cout << "test_shortest_path_found: passed\n";
}

REGISTER_QUERY_TEST(test_shortest_path_found)

void test_shortest_path_not_found() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "nonexistent_id";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);

  assert(!result.path_found);
  assert(!result.error_message.empty());

  std::cout << "test_shortest_path_not_found: passed\n";
}

REGISTER_QUERY_TEST(test_shortest_path_not_found)

void test_shortest_path_same_node() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "word_000001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);

  assert(result.path_found);
  assert(result.path_nodes.size() == 1);
  assert(result.path_edges.empty());
  assert(result.path_nodes[0].object_id == "word_000001");

  std::cout << "test_shortest_path_same_node: passed\n";
}

REGISTER_QUERY_TEST(test_shortest_path_same_node)

void test_shortest_path_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.target_id = "speaker_0001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::find_shortest_path(pkg, opts);
  auto json = svp::query::path_result_to_json(result);

  assert(json.contains("start_id"));
  assert(json.contains("target_id"));
  assert(json.contains("path_found"));
  assert(json.value("path_found", false) == true);
  assert(json.contains("path_length"));
  assert(json.contains("nodes"));
  assert(json.contains("edges"));
  assert(json["nodes"].is_array());
  assert(json["edges"].is_array());

  std::cout << "test_shortest_path_json_output: passed\n";
}

REGISTER_QUERY_TEST(test_shortest_path_json_output)
