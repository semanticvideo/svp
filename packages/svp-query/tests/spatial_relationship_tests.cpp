#include "query_test_registry.hpp"
#include "fixtures/spatial_pair_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>

void test_spatial_overlaps_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "region_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_overlaps = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "overlaps") {
      found_overlaps = true;
      break;
    }
  }
  assert(found_overlaps);

  std::cout << "test_spatial_overlaps_relationship: passed\n";
}

REGISTER_QUERY_TEST(test_spatial_overlaps_relationship)

void test_spatial_contains_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "region_000004";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_contains = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "contains") {
      found_contains = true;
      break;
    }
  }
  assert(found_contains);

  std::cout << "test_spatial_contains_relationship: passed\n";
}

REGISTER_QUERY_TEST(test_spatial_contains_relationship)

void test_spatial_near_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "region_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_near = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "near") {
      found_near = true;
      break;
    }
  }
  assert(found_near);

  std::cout << "test_spatial_near_relationship: passed\n";
}

REGISTER_QUERY_TEST(test_spatial_near_relationship)

void test_entity_enters_frame_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_enters = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "enters_frame" && edge.target_id == "frame_000001") {
      found_enters = true;
      break;
    }
  }
  assert(found_enters);

  std::cout << "test_entity_enters_frame_relationship: passed\n";
}

REGISTER_QUERY_TEST(test_entity_enters_frame_relationship)

void test_entity_exits_frame_relationship() {
  const auto pkg = create_spatial_pair_test_package();

  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_exits = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "exits_frame" && edge.target_id == "frame_000003") {
      found_exits = true;
      break;
    }
  }
  assert(found_exits);

  std::cout << "test_entity_exits_frame_relationship: passed\n";
}

REGISTER_QUERY_TEST(test_entity_exits_frame_relationship)
