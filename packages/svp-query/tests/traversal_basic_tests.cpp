#include "query_test_registry.hpp"
#include "fixtures/traversal_package_fixture.hpp"
#include "fixtures/basic_package_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>
#include <set>
#include <unordered_set>

void test_traversal_outgoing() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(result.start_id == "word_000001");
  assert(result.visited_node_count >= 2);

  bool found_speaker = false;
  bool found_seg = false;
  for (const auto& node : result.nodes) {
    if (node.object_id == "speaker_0001") found_speaker = true;
    if (node.object_id == "seg_001") found_seg = true;
  }
  assert(found_speaker);
  assert(found_seg);

  bool found_outgoing = false;
  bool found_incoming = false;
  for (const auto& edge : result.edges) {
    if (edge.direction == "outgoing") found_outgoing = true;
    if (edge.direction == "incoming") found_incoming = true;
  }
  assert(found_outgoing);
  assert(!found_incoming);

  std::cout << "test_traversal_outgoing: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_outgoing)

void test_traversal_incoming() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "speaker_0001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Incoming;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(result.visited_node_count >= 2);

  bool found_word1 = false;
  bool found_word2 = false;
  for (const auto& node : result.nodes) {
    if (node.object_id == "word_000001") found_word1 = true;
    if (node.object_id == "word_000002") found_word2 = true;
  }
  assert(found_word1);
  assert(found_word2);

  bool found_incoming = false;
  bool found_outgoing = false;
  for (const auto& edge : result.edges) {
    if (edge.direction == "incoming") found_incoming = true;
    if (edge.direction == "outgoing") found_outgoing = true;
  }
  assert(found_incoming);
  assert(!found_outgoing);

  std::cout << "test_traversal_incoming: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_incoming)

void test_traversal_both_directions() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool has_outgoing = false;
  bool has_incoming = false;
  for (const auto& edge : result.edges) {
    if (edge.direction == "outgoing") has_outgoing = true;
    if (edge.direction == "incoming") has_incoming = true;
  }
  assert(has_outgoing);
  assert(has_incoming);

  std::cout << "test_traversal_both_directions: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_both_directions)

void test_traversal_max_depth() {
  const auto pkg = create_traversal_test_package();

  svp::query::TraversalOptions opts1;
  opts1.start_id = "word_000001";
  opts1.max_depth = 1;
  opts1.direction = svp::query::TraversalDirection::Both;
  opts1.limit = 100;
  auto result1 = svp::query::traverse_relationships(pkg, opts1);

  for (const auto& node : result1.nodes) {
    assert(node.depth <= 1);
  }

  svp::query::TraversalOptions opts2;
  opts2.start_id = "word_000001";
  opts2.max_depth = 0;
  opts2.direction = svp::query::TraversalDirection::Both;
  opts2.limit = 100;
  auto result2 = svp::query::traverse_relationships(pkg, opts2);

  assert(result2.visited_node_count == 1);
  assert(result2.edges.empty());

  std::cout << "test_traversal_max_depth: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_max_depth)

void test_traversal_cycle_safety() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 10;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 1000;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(result.visited_node_count > 0);

  std::unordered_set<std::string> seen;
  for (const auto& node : result.nodes) {
    assert(seen.find(node.object_id) == seen.end());
    seen.insert(node.object_id);
  }

  std::cout << "test_traversal_cycle_safety: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_cycle_safety)

void test_traversal_class_filter() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "support";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  for (const auto& edge : result.edges) {
    assert(edge.relationship_class == "support");
  }

  bool found_appears_in_frame = false;
  for (const auto& edge : result.edges) {
    if (edge.relationship_type == "appears_in_frame") found_appears_in_frame = true;
  }
  assert(found_appears_in_frame);

  for (const auto& edge : result.edges) {
    assert(edge.relationship_type != "overlaps");
    assert(edge.relationship_type != "appears_in_shot");
  }

  std::cout << "test_traversal_class_filter: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_class_filter)

void test_traversal_type_filter() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.type_filter = std::string{"overlaps"};
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  for (const auto& edge : result.edges) {
    assert(edge.relationship_type == "overlaps");
  }
  assert(!result.edges.empty());

  std::cout << "test_traversal_type_filter: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_type_filter)

void test_traversal_unknown_type_traversable() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "unknown";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(!result.edges.empty());
  for (const auto& edge : result.edges) {
    assert(edge.relationship_class == "unknown");
    assert(edge.relationship_type == "totally_unknown_type");
  }

  std::cout << "test_traversal_unknown_type_traversable: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_unknown_type_traversable)

void test_traversal_missing_ids_reported() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000003";
  opts.max_depth = 1;
  opts.direction = svp::query::TraversalDirection::Outgoing;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(!result.missing_object_ids.empty());
  bool found_nonexistent = false;
  for (const auto& id : result.missing_object_ids) {
    if (id == "nonexistent_speaker") found_nonexistent = true;
  }
  assert(found_nonexistent);

  for (const auto& node : result.nodes) {
    if (node.object_id == "nonexistent_speaker") {
      assert(!node.resolved);
    }
  }

  std::cout << "test_traversal_missing_ids_reported: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_missing_ids_reported)

void test_traversal_embedding_id_resolves() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "text_obs_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  bool found_embed = false;
  bool embed_resolved = false;
  for (const auto& node : result.nodes) {
    if (node.object_id == "embed_text_obs_000001") {
      found_embed = true;
      embed_resolved = node.resolved;
      assert(node.source_layer == "embeddings/embeddings.index.jsonl");
    }
  }
  assert(found_embed);
  assert(embed_resolved);

  bool embed_in_missing = false;
  for (const auto& id : result.missing_object_ids) {
    if (id == "embed_text_obs_000001") embed_in_missing = true;
  }
  assert(!embed_in_missing);

  std::cout << "test_traversal_embedding_id_resolves: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_embedding_id_resolves)

void test_object_catalog_annotations() {
  const auto pkg = create_traversal_test_package();
  auto catalog = svp::query::build_object_catalog(pkg);

  const char* required_ids[] = {
      "word_000001", "text_obs_000001", "text_region_000001",
      "frame_000001", "entity_001", "crop_000001",
      "mask_000001", "depth_frame_000001", "embed_text_obs_000001"
  };
  for (const auto* id : required_ids) {
    const auto* entry = catalog.find(id);
    assert(entry != nullptr);
    assert(entry->object_id == id);
    assert(!entry->source_layer.empty());
  }

  const auto word_summary = catalog.node_summary("word_000001");
  assert(word_summary.value("kind", "") == "word");
  assert(word_summary.value("text", "") == "hello");

  const auto obs_summary = catalog.node_summary("text_obs_000001");
  assert(obs_summary.value("kind", "") == "text_observation");
  assert(obs_summary.value("raw_text", "") == "SALE $9.99");

  const auto region_summary = catalog.node_summary("text_region_000001");
  assert(region_summary.value("kind", "") == "text_region");

  const auto frame_summary = catalog.node_summary("frame_000001");
  assert(frame_summary.value("kind", "") == "frame");

  const auto entity_summary = catalog.node_summary("entity_001");
  assert(entity_summary.value("kind", "") == "entity");
  assert(entity_summary.value("entity_type", "") == "person");

  const auto crop_summary = catalog.node_summary("crop_000001");
  assert(crop_summary.value("kind", "") == "evidence_crop");
  assert(crop_summary.value("crop_file_path", "") == "text/evidence_crops/crop_000001.jpg");

  const auto mask_summary = catalog.node_summary("mask_000001");
  assert(mask_summary.value("kind", "") == "mask");
  assert(mask_summary.value("block_path", "") == "spatial/masks/mask_000001.bin");

  const auto depth_summary = catalog.node_summary("depth_frame_000001");
  assert(depth_summary.value("kind", "") == "depth");
  assert(depth_summary.value("block_path", "") == "spatial/depth/depth_000001.bin");

  const auto embed_summary = catalog.node_summary("embed_text_obs_000001");
  assert(embed_summary.value("kind", "") == "embedding");
  assert(embed_summary.value("source_type", "") == "text_observation");
  assert(embed_summary.value("source_id", "") == "text_obs_000001");

  std::cout << "test_object_catalog_annotations: passed\n";
}

REGISTER_QUERY_TEST(test_object_catalog_annotations)

void test_traversal_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);
  auto json = svp::query::traversal_result_to_json(result);

  assert(json.contains("start_id"));
  assert(json.value("start_id", "") == "word_000001");
  assert(json.contains("requested_depth"));
  assert(json.contains("visited_node_count"));
  assert(json.contains("edge_count"));
  assert(json.contains("limit_applied"));
  assert(json.contains("nodes"));
  assert(json.contains("edges"));
  assert(json["nodes"].is_array());
  assert(json["edges"].is_array());

  if (!json["nodes"].empty()) {
    const auto& first_node = json["nodes"][0];
    assert(first_node.contains("object_id"));
    assert(first_node.contains("depth"));
    assert(first_node.contains("resolved"));
  }

  if (!json["edges"].empty()) {
    const auto& first_edge = json["edges"][0];
    assert(first_edge.contains("relationship_type"));
    assert(first_edge.contains("relationship_class"));
    assert(first_edge.contains("source_id"));
    assert(first_edge.contains("target_id"));
    assert(first_edge.contains("direction"));
    assert(first_edge.contains("depth"));
  }

  std::cout << "test_traversal_json_output: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_json_output)

void test_traversal_empty_relationships() {
  const auto pkg = create_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  // Contract: empty relationships file = valid empty graph.
  // No error, no edges, start node is still visited and resolved.
  assert(result.error_message.empty());
  assert(result.edges.empty());
  assert(result.visited_node_count == 1);
  assert(result.nodes.size() == 1);
  assert(result.nodes[0].object_id == "word_000001");
  assert(result.nodes[0].resolved);

  std::cout << "test_traversal_empty_relationships: passed\n";
}

REGISTER_QUERY_TEST(test_traversal_empty_relationships)

void test_edge_deduplication() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "entity_001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());

  std::set<std::string> seen_edge_ids;
  for (const auto& edge : result.edges) {
    if (!edge.relationship_id.empty()) {
      assert(seen_edge_ids.find(edge.relationship_id) == seen_edge_ids.end());
      seen_edge_ids.insert(edge.relationship_id);
    }
  }

  std::cout << "test_edge_deduplication: passed\n";
}

REGISTER_QUERY_TEST(test_edge_deduplication)

void test_deterministic_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;

  auto result1 = svp::query::traverse_relationships(pkg, opts);
  auto result2 = svp::query::traverse_relationships(pkg, opts);

  auto json1 = svp::query::traversal_result_to_json(result1);
  auto json2 = svp::query::traversal_result_to_json(result2);

  assert(json1.dump() == json2.dump());

  std::cout << "test_deterministic_json_output: passed\n";
}

REGISTER_QUERY_TEST(test_deterministic_json_output)
