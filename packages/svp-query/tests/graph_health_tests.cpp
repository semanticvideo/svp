#include "query_test_registry.hpp"
#include "fixtures/traversal_package_fixture.hpp"
#include "fixtures/basic_package_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>

void test_graph_health_report() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.graph_health.total_edges > 0);
  assert(result.graph_health.total_nodes > 0);
  assert(result.graph_health.class_counts.find("support") != result.graph_health.class_counts.end() ||
         result.graph_health.class_counts.find("semantic") != result.graph_health.class_counts.end());

  auto json = svp::query::traversal_result_to_json(result);
  assert(json.contains("graph_health"));
  assert(json["graph_health"].contains("total_edges"));
  assert(json["graph_health"].contains("total_nodes"));
  assert(json["graph_health"].contains("class_counts"));

  std::cout << "test_graph_health_report: passed\n";
}

REGISTER_QUERY_TEST(test_graph_health_report)

void test_graph_health_diagnostics() {
  const auto pkg = create_traversal_test_package();

  auto health = svp::query::compute_graph_health(pkg);

  assert(health.total_edges > 0);
  assert(health.total_nodes > 0);
  assert(health.resolved_nodes > 0);
  assert(!health.class_counts.empty());
  assert(!health.type_counts.empty());

  auto json = svp::query::graph_health_to_json(health);
  assert(json.contains("total_edges"));
  assert(json.contains("total_nodes"));
  assert(json.contains("resolved_nodes"));
  assert(json.contains("unresolved_nodes"));
  assert(json.contains("orphan_nodes"));
  assert(json.contains("class_counts"));
  assert(json.contains("type_counts"));

  std::cout << "test_graph_health_diagnostics: passed\n";
}

REGISTER_QUERY_TEST(test_graph_health_diagnostics)

void test_graph_health_diagnostics_json_deterministic() {
  const auto pkg = create_traversal_test_package();

  auto health1 = svp::query::compute_graph_health(pkg);
  auto health2 = svp::query::compute_graph_health(pkg);

  auto json1 = svp::query::graph_health_to_json(health1);
  auto json2 = svp::query::graph_health_to_json(health2);

  assert(json1.dump() == json2.dump());

  std::cout << "test_graph_health_diagnostics_json_deterministic: passed\n";
}

REGISTER_QUERY_TEST(test_graph_health_diagnostics_json_deterministic)

void test_graph_health_enhanced_diagnostics() {
  const auto pkg = create_test_package();

  auto health = svp::query::compute_graph_health(pkg);
  auto j = svp::query::graph_health_to_json(health);

  // Verify new fields are present in JSON output.
  assert(j.contains("backend_used"));
  assert(j.contains("index_available"));
  assert(j.contains("mask_data_available"));
  assert(j.contains("depth_data_available"));

  // The test package has no mask or depth data, so skipped categories should
  // report them honestly.
  assert(j.contains("skipped_categories"));
  const auto& skipped = j["skipped_categories"];
  assert(skipped.is_array());
  assert(skipped.size() >= 2);

  bool found_mask_skip = false;
  bool found_depth_skip = false;
  bool found_motion_skip = false;
  for (const auto& cat : skipped) {
    const std::string category = cat["category"];
    const std::string reason = cat["reason"];
    if (category.find("occludes") != std::string::npos) found_mask_skip = true;
    if (category.find("foreground") != std::string::npos) found_depth_skip = true;
    if (category.find("moves_with") != std::string::npos) found_motion_skip = true;
    assert(!reason.empty());
  }
  assert(found_mask_skip);
  assert(found_depth_skip);
  assert(found_motion_skip);

  // Backend should be jsonl (no index.sqlite in test package).
  assert(j["backend_used"] == "jsonl");
  assert(j["index_available"] == false);

  std::cout << "test_graph_health_enhanced_diagnostics: passed\n";
}

REGISTER_QUERY_TEST(test_graph_health_enhanced_diagnostics)
