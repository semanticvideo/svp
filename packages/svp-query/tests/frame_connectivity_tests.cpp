#include "query_test_registry.hpp"
#include "fixtures/frame_connectivity_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>
#include <set>

void test_frame_shot_scene_connectivity() {
  const auto pkg = create_frame_connectivity_test_package();

  // Traverse from frame_000001 — should reach shot_000001 and scene_000001
  svp::query::TraversalOptions opts;
  opts.start_id = "frame_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_frame_in_shot = false;
  bool found_frame_in_scene = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "frame_in_shot" && edge.target_id == "shot_000001") {
      found_frame_in_shot = true;
    }
    if (edge.relationship_type == "frame_in_scene" && edge.target_id == "scene_000001") {
      found_frame_in_scene = true;
    }
  }
  assert(found_frame_in_shot);
  assert(found_frame_in_scene);

  // Verify shot_000001 and scene_000001 are in visited nodes
  std::set<std::string> visited_ids;
  for (const auto& node : result.nodes) {
    visited_ids.insert(node.object_id);
  }
  assert(visited_ids.count("shot_000001") > 0);
  assert(visited_ids.count("scene_000001") > 0);

  std::cout << "test_frame_shot_scene_connectivity: passed\n";
}

REGISTER_QUERY_TEST(test_frame_shot_scene_connectivity)

void test_frame_connectivity_all_frames() {
  const auto pkg = create_frame_connectivity_test_package();

  // Verify every frame has at least one outgoing edge to shot or scene
  const std::string frame_ids[] = {"frame_000001", "frame_000002", "frame_000003"};
  for (const auto& frame_id : frame_ids) {
    svp::query::TraversalOptions opts;
    opts.start_id = frame_id;
    opts.max_depth = 1;
    opts.direction = svp::query::TraversalDirection::Outgoing;
    opts.limit = 100;

    auto result = svp::query::traverse_relationships(pkg, opts);
    assert(result.error_message.empty());

    bool has_edge = false;
    for (const auto& edge : result.edges) {
      if (edge.relationship_type == "frame_in_shot" || edge.relationship_type == "frame_in_scene") {
        has_edge = true;
        break;
      }
    }
    assert(has_edge);
  }

  std::cout << "test_frame_connectivity_all_frames: passed\n";
}

REGISTER_QUERY_TEST(test_frame_connectivity_all_frames)

void test_frame_traversal_to_scene() {
  const auto pkg = create_frame_connectivity_test_package();

  // Traverse from frame_000003 (in shot_000002, scene_000001)
  // Verify it reaches scene_000001 via frame_in_scene
  svp::query::TraversalOptions opts;
  opts.start_id = "frame_000003";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.type_filter = "frame_in_scene";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  bool found_scene = false;
  for (const auto& edge : result.edges) {
    assert(edge.relationship_type == "frame_in_scene");
    if (edge.target_id == "scene_000001") {
      found_scene = true;
    }
  }
  assert(found_scene);

  std::cout << "test_frame_traversal_to_scene: passed\n";
}

REGISTER_QUERY_TEST(test_frame_traversal_to_scene)

void test_frame_in_shot_classified_as_support() {
  const auto pkg = create_frame_connectivity_test_package();

  // Filter by support class — frame_in_shot and frame_in_scene should appear
  svp::query::TraversalOptions opts;
  opts.start_id = "frame_000001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "support";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "frame_in_shot" || edge.relationship_type == "frame_in_scene") {
      found = true;
      break;
    }
  }
  assert(found);

  std::cout << "test_frame_in_shot_classified_as_support: passed\n";
}

REGISTER_QUERY_TEST(test_frame_in_shot_classified_as_support)
