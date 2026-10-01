// provenance/processors.jsonl composition (RC2 §18.1): every stage's processor
// records survive, in canonical order, whatever order the stage tasks finished
// in.

#include "processor_provenance.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace svp::builder;

int g_failures = 0;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

fs::path scratch_dir(const std::string& name) {
  std::random_device random;
  const fs::path path = fs::temp_directory_path() /
                        ("svp-processor-provenance-" + name + "-" +
                         std::to_string(random()));
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

nlohmann::json record(const std::string& id, const std::string& version = "v1") {
  return {{"id", id}, {"version", version}};
}

// One contribution per stage, ids deliberately not in stage order.
struct StageContribution {
  std::string_view stage;
  std::vector<nlohmann::json> records;
};

std::vector<StageContribution> stage_contributions() {
  return {
      {processor_fragment::kColor, {record("proc_z_color"), record("proc_m_timeline")}},
      {processor_fragment::kAudioExtract,
       {record("proc_b_audio_extraction"), record("proc_y_spectrum")}},
      {processor_fragment::kAudioTranscribe, {record("proc_k_diarization")}},
  };
}

std::vector<nlohmann::json> lane_records() {
  return {record("proc_q_ocr"), record("proc_a_tracker"), record("proc_r_depth")};
}

std::vector<std::string> ids_of(const std::vector<nlohmann::json>& records) {
  std::vector<std::string> ids;
  for (const nlohmann::json& value : records) {
    ids.push_back(value.at("id").get<std::string>());
  }
  return ids;
}

void test_merge_keeps_every_record_in_id_order_for_any_contribution_order() {
  std::vector<std::vector<nlohmann::json>> contributions;
  std::size_t total = 0;
  for (const StageContribution& stage : stage_contributions()) {
    contributions.push_back(stage.records);
    total += stage.records.size();
  }
  contributions.push_back(lane_records());
  total += lane_records().size();

  const ProcessorRecordMerge reference = merge_processor_records_canonically(contributions);
  require(reference.records.size() == total, "every stage's records survive the merge");
  require(reference.duplicates_merged == 0, "distinct ids are not merged");
  const std::vector<std::string> ids = ids_of(reference.records);
  require(std::is_sorted(ids.begin(), ids.end()), "merged records are sorted by id");

  std::vector<std::size_t> order(contributions.size());
  std::iota(order.begin(), order.end(), 0);
  std::size_t permutations = 0;
  do {
    std::vector<std::vector<nlohmann::json>> permuted;
    for (const std::size_t index : order) {
      std::vector<nlohmann::json> stage = contributions[index];
      std::reverse(stage.begin(), stage.end());
      permuted.push_back(std::move(stage));
    }
    require(merge_processor_records_canonically(permuted).records == reference.records,
            "the merge does not depend on contribution order");
    ++permutations;
  } while (std::next_permutation(order.begin(), order.end()));
  require(permutations > 1, "more than one contribution order was checked");
}

void test_merge_resolves_duplicate_ids_independent_of_order() {
  const nlohmann::json smaller = record("proc_shared", "v1");
  const nlohmann::json larger = record("proc_shared", "v2");
  const nlohmann::json no_id = {{"name", "record without an id"}};
  const ProcessorRecordMerge forward =
      merge_processor_records_canonically({{smaller}, {larger, no_id}});
  const ProcessorRecordMerge backward =
      merge_processor_records_canonically({{no_id, larger}, {smaller}});
  require(forward.records == backward.records, "duplicate resolution ignores order");
  require(forward.records.size() == 1 && forward.records.front() == smaller,
          "the smallest serialization of a duplicated id is kept");
  require(forward.duplicates_merged == 1 && backward.duplicates_merged == 1,
          "a duplicated id is counted once");
}

// Stages write their fragments in `stage_order` (the order their tasks
// finished), then the entities stage composes processors.jsonl.
std::string compose_after(const std::vector<std::size_t>& stage_order,
                          const std::string& name) {
  const fs::path staging = scratch_dir(name);
  // An earlier processors.jsonl (for example, a reused staging directory) is
  // replaced, not merged.
  fs::create_directories(staging / "provenance");
  std::ofstream(staging / "provenance" / "processors.jsonl") << record("stale").dump() << "\n";

  const std::vector<StageContribution> stages = stage_contributions();
  for (const std::size_t index : stage_order) {
    write_stage_processor_fragment(staging, stages[index].stage, stages[index].records);
  }
  const ProcessorRecordMerge merge = compose_processor_provenance(staging, lane_records());
  require(!fs::exists(staging / kStageProcessorFragmentDir),
          "composition removes the stage fragments");
  const std::string written = read_text(staging / "provenance" / "processors.jsonl");
  std::string expected;
  for (const nlohmann::json& value : merge.records) {
    expected += value.dump() + "\n";
  }
  require(written == expected, "processors.jsonl holds the merged records");
  require(written.find("\"stale\"") == std::string::npos,
          "an earlier processors.jsonl is replaced");
  fs::remove_all(staging);
  return written;
}

void test_compose_keeps_every_stage_for_any_completion_order() {
  std::vector<std::size_t> order(stage_contributions().size());
  std::iota(order.begin(), order.end(), 0);
  const std::string reference = compose_after(order, "reference");
  for (const StageContribution& stage : stage_contributions()) {
    for (const nlohmann::json& value : stage.records) {
      require(reference.find(value.dump()) != std::string::npos,
              "a record of stage " + std::string(stage.stage) + " survives");
    }
  }
  for (const nlohmann::json& value : lane_records()) {
    require(reference.find(value.dump()) != std::string::npos,
            "a vision lane record survives");
  }
  while (std::next_permutation(order.begin(), order.end())) {
    require(compose_after(order, "permuted") == reference,
            "processors.jsonl does not depend on stage completion order");
  }
}

void test_fragment_refs_are_per_stage() {
  const std::string color = stage_processor_fragment_ref(processor_fragment::kColor);
  require(color.starts_with(kStageProcessorFragmentDir),
          "fragments live in the fragment directory");
  require(color != stage_processor_fragment_ref(processor_fragment::kAudioExtract),
          "each stage has its own fragment");
}

}  // namespace

int main() {
  test_merge_keeps_every_record_in_id_order_for_any_contribution_order();
  test_merge_resolves_duplicate_ids_independent_of_order();
  test_compose_keeps_every_stage_for_any_completion_order();
  test_fragment_refs_are_per_stage();
  if (g_failures != 0) {
    std::cerr << g_failures << " processor provenance test(s) failed\n";
    return 1;
  }
  std::cout << "processor provenance tests passed\n";
  return 0;
}
