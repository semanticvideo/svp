#include "query_test_registry.hpp"
#include "fixtures/basic_package_fixture.hpp"
#include "svp/query/query_reader.hpp"
#include "svp/query/query_ops.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <iostream>

void test_read_jsonl_entry() {
  const auto pkg = create_test_package();
  auto result = svp::query::read_jsonl_entry(pkg, "transcript/words.jsonl");

  assert(result.present);
  assert(result.readable);
  assert(result.records.size() == 5);
  assert(result.records[0].value("text", "") == "hello");

  auto missing = svp::query::read_jsonl_entry(pkg, "nonexistent/file.jsonl");
  assert(!missing.present);
  assert(!missing.readable);

  std::cout << "test_read_jsonl_entry: passed\n";
}

REGISTER_QUERY_TEST(test_read_jsonl_entry)

void test_read_json_entry() {
  const auto pkg = create_test_package();
  auto result = svp::query::read_json_entry(pkg, "transcript/transcript.json");

  assert(result.present);
  assert(result.readable);
  assert(result.parsed);
  assert(result.value.value("word_count", 0) == 5);

  std::cout << "test_read_json_entry: passed\n";
}

REGISTER_QUERY_TEST(test_read_json_entry)

void test_list_layers() {
  const auto pkg = create_test_package();
  auto summary = svp::query::list_layers(pkg);

  assert(!summary.layers.empty());
  assert(summary.total_entries > 0);

  bool found_words = false;
  bool found_colors = false;
  bool found_validation = false;
  for (const auto& layer : summary.layers) {
    if (layer.entry == "transcript/words.jsonl" && layer.present) {
      assert(layer.record_count == 5);
      found_words = true;
    }
    if (layer.entry == "colors/color_observations.jsonl" && layer.present) {
      assert(layer.record_count == 2);
      found_colors = true;
    }
    if (layer.entry == "provenance/validation.json" && layer.present) {
      assert(layer.record_count == 1);
      found_validation = true;
    }
  }
  assert(found_words);
  assert(found_colors);
  assert(found_validation);

  std::cout << "test_list_layers: passed\n";
}

REGISTER_QUERY_TEST(test_list_layers)

void test_transcript_summary() {
  const auto pkg = create_test_package();
  auto result = svp::query::transcript_summary(pkg);

  assert(result.present);
  assert(result.word_count_file == 5);
  assert(result.speaker_count_file == 1);

  const auto lang = result.transcript_json.value("language", nlohmann::json{});
  assert(lang.value("primary", "") == "en");

  std::cout << "test_transcript_summary: passed\n";
}

REGISTER_QUERY_TEST(test_transcript_summary)

void test_find_words() {
  const auto pkg = create_test_package();

  auto matches = svp::query::find_words(pkg, "cam", 100);
  assert(matches.size() == 1);
  assert(matches[0].record.value("text", "") == "camera");

  auto matches2 = svp::query::find_words(pkg, "o", 100);
  assert(matches2.size() == 3);

  auto matches3 = svp::query::find_words(pkg, "nonexistent", 100);
  assert(matches3.empty());

  std::cout << "test_find_words: passed\n";
}

REGISTER_QUERY_TEST(test_find_words)

void test_list_speakers() {
  const auto pkg = create_test_package();
  auto speakers = svp::query::list_speakers(pkg);

  assert(speakers.size() == 1);
  assert(speakers[0].record.value("id", "") == "speaker_0001");
  assert(speakers[0].word_count == 5);

  std::cout << "test_list_speakers: passed\n";
}

REGISTER_QUERY_TEST(test_list_speakers)

void test_list_ocr_observations() {
  const auto pkg = create_test_package();

  auto all = svp::query::list_ocr_observations(pkg, std::nullopt, 100);
  assert(all.size() == 2);

  auto filtered = svp::query::list_ocr_observations(pkg, std::string{"sale"}, 100);
  assert(filtered.size() == 1);
  assert(filtered[0].record.value("raw_text", "") == "SALE $9.99");

  auto with_crops = svp::query::list_ocr_observations(pkg, std::nullopt, 100);
  bool has_crop_ref = false;
  for (const auto& obs : with_crops) {
    const auto refs = obs.record.find("evidence_crop_refs");
    if (refs != obs.record.end() && refs->is_array() && !refs->empty()) {
      has_crop_ref = true;
    }
  }
  assert(has_crop_ref);

  std::cout << "test_list_ocr_observations: passed\n";
}

REGISTER_QUERY_TEST(test_list_ocr_observations)

void test_list_color_observations() {
  const auto pkg = create_test_package();

  auto all = svp::query::list_color_observations(pkg, std::nullopt, std::nullopt, 100);
  assert(all.size() == 2);

  auto orange = svp::query::list_color_observations(pkg, std::string{"orange"}, std::nullopt, 100);
  assert(orange.size() == 1);
  assert(orange[0].record.value("dominant_bucket", "") == "orange");

  auto threshold = svp::query::list_color_observations(pkg, std::nullopt, std::make_optional(0.5), 100);
  assert(threshold.size() == 1);
  assert(threshold[0].record.value("dominant_bucket", "") == "orange");

  std::cout << "test_list_color_observations: passed\n";
}

REGISTER_QUERY_TEST(test_list_color_observations)

void test_show_validation() {
  const auto pkg = create_test_package();
  auto info = svp::query::show_validation(pkg);

  assert(info.present);
  assert(info.record.value("status", "") == "valid");
  assert(info.record.value("core_status", "") == "valid");

  std::cout << "test_show_validation: passed\n";
}

REGISTER_QUERY_TEST(test_show_validation)
