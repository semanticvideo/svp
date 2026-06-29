#include "query_test_registry.hpp"
#include "fixtures/basic_package_fixture.hpp"
#include "fixtures/malformed_package_fixture.hpp"
#include "svp/query/query_reader.hpp"
#include "svp/query/query_ops.hpp"

#include <cassert>
#include <iostream>

void test_malformed_jsonl() {
  const auto pkg = create_malformed_package();
  auto result = svp::query::read_jsonl_entry(pkg, "transcript/words.jsonl");

  assert(result.present);
  assert(result.readable);
  assert(result.has_malformed);
  assert(result.malformed_line_count == 1);
  assert(!result.error_message.empty());
  assert(result.records.size() == 2);

  // Verify error message contains useful detail
  assert(result.error_message.find("malformed") != std::string::npos);
  assert(result.error_message.find("line 2") != std::string::npos);

  // Happy path: valid JSONL should not have malformed flag
  const auto good_pkg = create_test_package();
  auto good_result = svp::query::read_jsonl_entry(good_pkg, "transcript/words.jsonl");
  assert(good_result.readable);
  assert(!good_result.has_malformed);
  assert(good_result.malformed_line_count == 0);
  assert(good_result.error_message.empty());

  std::cout << "test_malformed_jsonl: passed\n";
}

REGISTER_QUERY_TEST(test_malformed_jsonl)

void test_malformed_transcript_json() {
  const auto pkg = create_malformed_package();
  auto result = svp::query::transcript_summary(pkg);

  assert(result.present);
  assert(!result.parsed);
  assert(!result.error_message.empty());

  // Happy path: valid transcript should parse
  const auto good_pkg = create_test_package();
  auto good_result = svp::query::transcript_summary(good_pkg);
  assert(good_result.present);
  assert(good_result.parsed);
  assert(good_result.error_message.empty());

  std::cout << "test_malformed_transcript_json: passed\n";
}

REGISTER_QUERY_TEST(test_malformed_transcript_json)

void test_malformed_validation_json() {
  const auto pkg = create_malformed_package();
  auto info = svp::query::show_validation(pkg);

  assert(info.present);
  assert(!info.parsed);
  assert(!info.error_message.empty());

  // Happy path: valid validation should parse
  const auto good_pkg = create_test_package();
  auto good_info = svp::query::show_validation(good_pkg);
  assert(good_info.present);
  assert(good_info.parsed);
  assert(good_info.error_message.empty());
  assert(good_info.record.value("status", "") == "valid");

  std::cout << "test_malformed_validation_json: passed\n";
}

REGISTER_QUERY_TEST(test_malformed_validation_json)

void test_malformed_layers() {
  const auto pkg = create_malformed_package();
  auto summary = svp::query::list_layers(pkg);

  bool found_malformed_words = false;
  bool found_malformed_transcript = false;
  bool found_malformed_validation = false;
  bool found_mimetype_text = false;

  for (const auto& layer : summary.layers) {
    if (layer.entry == "transcript/words.jsonl") {
      assert(layer.present);
      assert(layer.has_malformed);
      assert(layer.malformed_line_count == 1);
      found_malformed_words = true;
    }
    if (layer.entry == "transcript/transcript.json") {
      assert(layer.present);
      assert(layer.has_malformed);
      found_malformed_transcript = true;
    }
    if (layer.entry == "provenance/validation.json") {
      assert(layer.present);
      assert(layer.has_malformed);
      found_malformed_validation = true;
    }
    if (layer.entry == "mimetype") {
      assert(layer.kind == "text");
      found_mimetype_text = true;
    }
  }

  assert(found_malformed_words);
  assert(found_malformed_transcript);
  assert(found_malformed_validation);
  assert(found_mimetype_text);

  std::cout << "test_malformed_layers: passed\n";
}

REGISTER_QUERY_TEST(test_malformed_layers)
