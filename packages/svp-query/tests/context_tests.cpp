#include "query_test_registry.hpp"
#include "fixtures/traversal_package_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>

void test_context_mode() {
  const auto pkg = create_traversal_test_package();
  auto result = svp::query::build_context(pkg, "word_000001", 100);

  assert(result.object_id == "word_000001");
  assert(result.resolved);
  assert(!result.context_edges.empty());

  bool found_speaker = false;
  for (const auto& node : result.context_nodes) {
    if (node.object_id == "speaker_0001") found_speaker = true;
  }
  assert(found_speaker);

  std::cout << "test_context_mode: passed\n";
}

REGISTER_QUERY_TEST(test_context_mode)

void test_context_json_output() {
  const auto pkg = create_traversal_test_package();
  auto result = svp::query::build_context(pkg, "word_000001", 100);
  auto json = svp::query::context_result_to_json(result);

  assert(json.contains("object_id"));
  assert(json.contains("resolved"));
  assert(json.value("resolved", false) == true);
  assert(json.contains("context_nodes"));
  assert(json.contains("context_edges"));
  assert(json["context_nodes"].is_array());
  assert(json["context_edges"].is_array());

  std::cout << "test_context_json_output: passed\n";
}

REGISTER_QUERY_TEST(test_context_json_output)

void test_context_unresolved_object() {
  const auto pkg = create_traversal_test_package();
  auto result = svp::query::build_context(pkg, "nonexistent_id", 100);

  assert(result.object_id == "nonexistent_id");
  assert(!result.resolved);

  std::cout << "test_context_unresolved_object: passed\n";
}

REGISTER_QUERY_TEST(test_context_unresolved_object)

void test_context_with_depth() {
  const auto pkg = create_traversal_test_package();

  svp::query::ContextOptions opts;
  opts.object_id = "word_000001";
  opts.max_depth = 2;
  opts.limit = 100;

  auto result = svp::query::build_context(pkg, opts);

  assert(result.object_id == "word_000001");
  assert(result.resolved);
  assert(!result.context_edges.empty());

  bool found_speaker = false;
  for (const auto& node : result.context_nodes) {
    if (node.object_id == "speaker_0001") found_speaker = true;
  }
  assert(found_speaker);

  std::cout << "test_context_with_depth: passed\n";
}

REGISTER_QUERY_TEST(test_context_with_depth)

void test_context_with_time_window() {
  const auto pkg = create_traversal_test_package();

  svp::query::ContextOptions opts;
  opts.object_id = "word_000001";
  opts.max_depth = 2;
  opts.limit = 100;
  opts.at_us = 200000;

  auto result = svp::query::build_context(pkg, opts);

  assert(result.object_id == "word_000001");
  assert(result.resolved);

  for (const auto& edge : result.context_edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start <= 200000 && 200000 < end);
  }

  std::cout << "test_context_with_time_window: passed\n";
}

REGISTER_QUERY_TEST(test_context_with_time_window)

void test_context_at_and_start_mutually_exclusive() {
  const auto pkg = create_traversal_test_package();

  svp::query::ContextOptions opts;
  opts.object_id = "word_000001";
  opts.at_us = 100;
  opts.start_us = 0;

  auto result = svp::query::build_context(pkg, opts);

  assert(!result.error_message.empty());
  assert(result.error_message.find("cannot specify both") != std::string::npos);

  std::cout << "test_context_at_and_start_mutually_exclusive: passed\n";
}

REGISTER_QUERY_TEST(test_context_at_and_start_mutually_exclusive)
