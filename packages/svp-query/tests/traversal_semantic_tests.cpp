#include "query_test_registry.hpp"
#include "fixtures/traversal_package_fixture.hpp"
#include "fixtures/semantic_package_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>

void test_semantic_relationships_generated() {
  const auto pkg = create_semantic_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_appears_in_shot = false;
  bool found_appears_in_scene = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "appears_in_shot") found_appears_in_shot = true;
    if (edge.relationship_type == "appears_in_scene") found_appears_in_scene = true;
  }
  assert(found_appears_in_shot);
  assert(found_appears_in_scene);

  std::cout << "test_semantic_relationships_generated: passed\n";
}

REGISTER_QUERY_TEST(test_semantic_relationships_generated)

void test_semantic_visible_during_speech() {
  const auto pkg = create_semantic_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "text_region_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_visible_during_speech = false;
  bool found_visible_during_word_range = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "visible_during_speech") found_visible_during_speech = true;
    if (edge.relationship_type == "visible_during_word_range") found_visible_during_word_range = true;
  }
  assert(found_visible_during_speech);
  assert(found_visible_during_word_range);

  std::cout << "test_semantic_visible_during_speech: passed\n";
}

REGISTER_QUERY_TEST(test_semantic_visible_during_speech)

void test_semantic_speaker_active_during_entity_visible() {
  const auto pkg = create_semantic_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "seg_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_speaker_active = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "speaker_active_during_entity_visible") {
      found_speaker_active = true;
    }
  }
  assert(found_speaker_active);

  std::cout << "test_semantic_speaker_active_during_entity_visible: passed\n";
}

REGISTER_QUERY_TEST(test_semantic_speaker_active_during_entity_visible)

void test_unresolved_ids_in_graph_health() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 3;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  bool has_unresolved = false;
  for (const auto& node : result.nodes) {
    if (!node.resolved) {
      has_unresolved = true;
      break;
    }
  }

  if (has_unresolved) {
    assert(!result.missing_object_ids.empty());
  }

  auto json = svp::query::traversal_result_to_json(result);
  if (json.contains("missing_object_ids")) {
    assert(json["missing_object_ids"].is_array());
  }

  std::cout << "test_unresolved_ids_in_graph_health: passed\n";
}

REGISTER_QUERY_TEST(test_unresolved_ids_in_graph_health)

void test_zero_pts_semantic_relationships() {
  const auto pkg = create_zero_pts_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "semantic";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_appears_in_shot = false;
  bool found_appears_in_scene = false;
  bool found_visible_during_speech = false;
  bool found_speaker_active = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "appears_in_shot") found_appears_in_shot = true;
    if (edge.relationship_type == "appears_in_scene") found_appears_in_scene = true;
    if (edge.relationship_type == "visible_during_speech") found_visible_during_speech = true;
    if (edge.relationship_type == "speaker_active_during_entity_visible") found_speaker_active = true;
  }
  assert(found_appears_in_shot);
  assert(found_appears_in_scene);
  assert(found_visible_during_speech);
  assert(found_speaker_active);

  std::cout << "test_zero_pts_semantic_relationships: passed\n";
}

REGISTER_QUERY_TEST(test_zero_pts_semantic_relationships)
