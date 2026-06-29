#include "query_test_registry.hpp"
#include "fixtures/traversal_package_fixture.hpp"
#include "svp/query/traversal.hpp"

#include <cassert>
#include <iostream>

void test_time_window_at_us() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;
  opts.time_window.at_us = 0;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  assert(!result.edges.empty());

  for (const auto& edge : result.edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start <= 0 && 0 < end);
  }

  std::cout << "test_time_window_at_us: passed\n";
}

REGISTER_QUERY_TEST(test_time_window_at_us)

void test_time_window_start_end() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;
  opts.time_window.start_us = 0;
  opts.time_window.end_us = 500000;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  for (const auto& edge : result.edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start < 500000 && 0 < end);
  }

  std::cout << "test_time_window_start_end: passed\n";
}

REGISTER_QUERY_TEST(test_time_window_start_end)

void test_time_window_at_us_excludes_non_overlapping() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.direction = svp::query::TraversalDirection::Both;
  opts.class_filter = "all";
  opts.limit = 100;
  opts.time_window.at_us = 1200000;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(result.error_message.empty());
  for (const auto& edge : result.edges) {
    const auto start = edge.record.value("start_us", 0);
    const auto end = edge.record.value("end_us", 0);
    assert(start <= 1200000 && 1200000 < end);
  }

  std::cout << "test_time_window_at_us_excludes_non_overlapping: passed\n";
}

REGISTER_QUERY_TEST(test_time_window_at_us_excludes_non_overlapping)

void test_time_window_at_and_start_mutually_exclusive() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 2;
  opts.limit = 100;
  opts.time_window.at_us = 100;
  opts.time_window.start_us = 0;

  auto result = svp::query::traverse_relationships(pkg, opts);

  assert(!result.error_message.empty());
  assert(result.error_message.find("cannot specify both") != std::string::npos);

  std::cout << "test_time_window_at_and_start_mutually_exclusive: passed\n";
}

REGISTER_QUERY_TEST(test_time_window_at_and_start_mutually_exclusive)

void test_time_window_in_json_output() {
  const auto pkg = create_traversal_test_package();
  svp::query::TraversalOptions opts;
  opts.start_id = "word_000001";
  opts.max_depth = 1;
  opts.limit = 100;
  opts.time_window.at_us = 500000;

  auto result = svp::query::traverse_relationships(pkg, opts);
  auto json = svp::query::traversal_result_to_json(result);

  assert(json.contains("time_window"));
  assert(json["time_window"].contains("at_us"));
  assert(json["time_window"].value("at_us", 0) == 500000);

  std::cout << "test_time_window_in_json_output: passed\n";
}

REGISTER_QUERY_TEST(test_time_window_in_json_output)
