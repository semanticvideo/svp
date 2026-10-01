// The builder's task-engine pieces that decide resume correctness without
// running a build: the stage task plan (dependencies, staging scopes, width),
// staging capture and restore, the stage output encoding, frame catalog
// deltas, and the canonical frames codec.

#include "engine/build_assembly.hpp"
#include "engine/canonical_frames_codec.hpp"
#include "engine/frame_catalog_delta.hpp"
#include "engine/stage_task_plan.hpp"
#include "engine/stage_task_products.hpp"
#include "engine/staging_scope.hpp"

#include "svp/exec/task_graph.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace svp::builder::engine;

int g_failures = 0;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

fs::path scratch_dir(const std::string& name) {
  std::random_device random;
  const fs::path path =
      fs::temp_directory_path() / ("svp-task-engine-" + name + "-" + std::to_string(random()));
  fs::remove_all(path);
  fs::create_directories(path);
  return path;
}

void write_text(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

StageTaskPlanInputs package_inputs() {
  StageTaskPlanInputs inputs;
  inputs.stage_plan =
      svp::builder::execution_plan_for_stage(svp::builder::BuildStage::package_skeleton);
  inputs.single_video_heavy_lanes = 2;
  return inputs;
}

std::size_t position(const std::vector<PlannedStageTask>& tasks, StageTaskKind kind) {
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (tasks[i].kind == kind) return i;
  }
  return tasks.size();
}

bool depends_on(const std::vector<PlannedStageTask>& tasks, StageTaskKind task,
                StageTaskKind dependency) {
  const auto& deps = tasks[position(tasks, task)].depends_on;
  return std::find(deps.begin(), deps.end(), dependency) != deps.end();
}

void test_package_plan_overlaps_lanes() {
  const auto tasks = plan_stage_tasks(package_inputs());
  require(tasks.size() == 14, "package build plans 14 stage tasks");
  require(tasks.front().kind == StageTaskKind::inventory, "inventory runs first");
  require(tasks.back().kind == StageTaskKind::package_write, "package write runs last");
  // Audio lane, depth beside OCR: three tasks can run at once.
  require(stage_task_graph_width(tasks) == 3, "overlapping lanes give width 3");
  require(depends_on(tasks, StageTaskKind::tracking, StageTaskKind::audio_extract),
          "tracking waits for the processors.jsonl rewrite of audio extraction");
  require(!depends_on(tasks, StageTaskKind::canonical_frames,
                      StageTaskKind::audio_transcribe),
          "vision lane does not wait for audio when lanes overlap");
}

void test_serial_plan_is_one_chain() {
  StageTaskPlanInputs inputs = package_inputs();
  inputs.serial_pipeline = true;
  const auto tasks = plan_stage_tasks(inputs);
  require(stage_task_graph_width(tasks) == 1, "--serial plans a single chain");
  require(position(tasks, StageTaskKind::tracking) <
              position(tasks, StageTaskKind::audio_extract),
          "--serial keeps today's order: vision lane before audio lane");
  require(depends_on(tasks, StageTaskKind::ocr, StageTaskKind::depth),
          "--serial runs depth before OCR");
}

void test_single_heavy_lane_runs_audio_first() {
  StageTaskPlanInputs inputs = package_inputs();
  inputs.single_video_heavy_lanes = 1;
  const auto tasks = plan_stage_tasks(inputs);
  require(depends_on(tasks, StageTaskKind::canonical_frames,
                     StageTaskKind::audio_transcribe),
          "one heavy lane: the vision lane waits for the audio lane");
  require(stage_task_graph_width(tasks) == 2, "one heavy lane keeps depth beside OCR");
}

void test_microphone_mode_orders_processor_writers() {
  StageTaskPlanInputs inputs = package_inputs();
  inputs.microphone_stream_mode = true;
  const auto tasks = plan_stage_tasks(inputs);
  require(depends_on(tasks, StageTaskKind::tracking, StageTaskKind::audio_transcribe),
          "microphone transcription appends processors.jsonl before tracking merges it");
}

void test_svpi_and_stop_after_plans() {
  StageTaskPlanInputs inputs = package_inputs();
  inputs.svpi_publication = true;
  const auto svpi = plan_stage_tasks(inputs);
  require(svpi.size() == 16 && svpi.back().kind == StageTaskKind::svpi_write,
          "interlace create adds binding and SVPI write");

  StageTaskPlanInputs audio;
  audio.stage_plan = svp::builder::execution_plan_for_stage(svp::builder::BuildStage::audio);
  require(plan_stage_tasks(audio).size() == 3, "--stop-after audio: inventory + audio lane");
  StageTaskPlanInputs ingest;
  ingest.stage_plan =
      svp::builder::execution_plan_for_stage(svp::builder::BuildStage::media_ingest);
  require(plan_stage_tasks(ingest).size() == 1, "--stop-after media-ingest: inventory only");
}

void test_graph_cache_keys_follow_build_inputs() {
  const auto tasks = plan_stage_tasks(package_inputs());
  const svp::exec::TaskGraph a = make_stage_task_graph(tasks, "bs_test", std::string(64, 'a'));
  const svp::exec::TaskGraph b = make_stage_task_graph(tasks, "bs_test", std::string(64, 'a'));
  const svp::exec::TaskGraph c = make_stage_task_graph(tasks, "bs_test", std::string(64, 'b'));
  require(a.size() == tasks.size(), "graph has a node per task");
  for (std::size_t i = 0; i < a.size(); ++i) {
    require(a.node(i).spec.cache_key == b.node(i).spec.cache_key,
            "same build inputs give the same cache key");
    require(a.node(i).spec.cache_key != c.node(i).spec.cache_key,
            "different build inputs give a different cache key");
  }
}

void test_staging_capture_and_restore() {
  const fs::path source = scratch_dir("capture");
  write_text(source / "text" / "a.jsonl", "a\n");
  write_text(source / "text" / "crops" / "c.jpg", "jpeg");
  fs::create_directories(source / "text" / "empty");
  write_text(source / "spatial" / "depth.index.jsonl", "d\n");
  write_text(source / "spatial" / "masks.index.jsonl", "m\n");
  const StagingScope text{{"text/"}};
  const StagingScope depth{{"spatial/depth."}};

  const auto captured = capture_staging_scope(source, text);
  require(captured.size() == 5, "text/ capture holds 3 directories and 2 files");

  // Restore into a staging directory that holds stale and foreign entries.
  const fs::path target = scratch_dir("restore");
  write_text(target / "text" / "stale.jsonl", "old");
  write_text(target / "spatial" / "masks.index.jsonl", "foreign");
  restore_staging_scope(target, text, captured);
  require(!fs::exists(target / "text" / "stale.jsonl"), "restore removes stale scope files");
  require(fs::is_directory(target / "text" / "empty"), "restore recreates empty directories");
  require(read_text(target / "text" / "crops" / "c.jpg") == "jpeg", "restore writes files");
  require(read_text(target / "spatial" / "masks.index.jsonl") == "foreign",
          "restore leaves entries outside the scope alone");

  require(scopes_overlap(StagingScope{{"provenance/"}},
                         StagingScope{{"provenance/processors.jsonl"}}),
          "a directory scope overlaps a file inside it");
  require(!scopes_overlap(depth, StagingScope{{"spatial/masks."}}),
          "file-prefix scopes in one directory are disjoint");
  fs::remove_all(source);
  fs::remove_all(target);
}

void test_stage_products_round_trip() {
  StageTaskProducts products;
  products.staging.push_back({.relative_path = "text/", .kind = StagedEntryKind::directory});
  products.staging.push_back(
      {.relative_path = "text/a.jsonl", .kind = StagedEntryKind::file,
       .bytes = json_state_bytes(nlohmann::json{{"a", 1}})});
  products.staging.push_back(
      {.relative_path = "text/empty.jsonl", .kind = StagedEntryKind::file, .bytes = {}});
  products.states["foundation"] = json_state_bytes({{"key", "value"}});
  const StageTaskProducts copy = products;

  const EncodedStageProducts encoded = encode_stage_products(products);
  require(encoded.outputs.size() == 4, "manifest, two files, one state");
  require(encoded.outputs.front().role == "stage_manifest", "manifest comes first");
  const StageTaskProducts decoded = decode_stage_products(encoded.outputs, encoded.payloads);
  require(decoded.staging.size() == copy.staging.size(), "staging entries survive");
  for (std::size_t i = 0; i < decoded.staging.size(); ++i) {
    require(decoded.staging[i].relative_path == copy.staging[i].relative_path &&
                decoded.staging[i].bytes == copy.staging[i].bytes,
            "staging entry " + copy.staging[i].relative_path + " round-trips");
  }
  require(parse_json_state(decoded.states.at("foundation")) ==
              nlohmann::json{{"key", "value"}},
          "states round-trip");
}

svp::vision::FrameCatalog planned_catalog() {
  svp::vision::FrameCatalog catalog;
  catalog.register_frame(1000, "color");
  catalog.register_frame(2000, "canonical");
  catalog.register_frame(3000, "ocr");
  catalog.lock_to_plan();
  return catalog;
}

void test_frame_catalog_deltas_replay_in_any_order() {
  svp::vision::FrameCatalog color = planned_catalog();
  color.register_frame(1000, "color", true);
  svp::vision::FrameCatalog ocr = planned_catalog();
  ocr.register_frame(3000, "ocr");
  ocr.register_frame(1000, "ocr");

  svp::vision::FrameCatalog direct = planned_catalog();
  direct.register_frame(1000, "color", true);
  direct.register_frame(3000, "ocr");
  direct.register_frame(1000, "ocr");

  const auto same = [](const svp::vision::FrameCatalog& left,
                       const svp::vision::FrameCatalog& right) {
    const auto a = left.planned_entries();
    const auto b = right.planned_entries();
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].frame_id != b[i].frame_id || a[i].decoded != b[i].decoded ||
          a[i].keyframe != b[i].keyframe || a[i].purposes != b[i].purposes) {
        return false;
      }
    }
    return true;
  };
  svp::vision::FrameCatalog forward = planned_catalog();
  apply_frame_catalog_delta(forward, frame_catalog_delta(color));
  apply_frame_catalog_delta(forward, frame_catalog_delta(ocr));
  svp::vision::FrameCatalog backward = planned_catalog();
  apply_frame_catalog_delta(backward, frame_catalog_delta(ocr));
  apply_frame_catalog_delta(backward, frame_catalog_delta(color));
  require(same(forward, direct), "replayed deltas equal direct registration");
  require(same(backward, direct), "delta replay order does not matter");
  require(direct.entries().size() == 2, "the undecoded planned frame stays undecoded");
}

void test_canonical_frames_round_trip() {
  svp::vision::DecodedCanonicalFrames frames;
  frames.decoding_attempted = true;
  frames.decoding_succeeded = true;
  frames.frames_attempted = 2;
  frames.frames_decoded = 1;
  frames.frames_missed = 1;
  frames.skipped_reason = "one miss";
  svp::vision::ColorRasterFrame frame;
  frame.frame_id = "frame_000002";
  frame.timestamp_us = 2000;
  frame.width = 2;
  frame.height = 1;
  frame.keyframe = true;
  frame.frame_index = 1;
  frame.pixels = {{1, 2, 3}, {4, 5, 6}};
  frames.frames.push_back(frame);

  const EncodedCanonicalFrames encoded = encode_canonical_frames(frames);
  const auto decoded = decode_canonical_frames_state(
      parse_json_state(json_state_bytes(encoded.index)), encoded.pixels);
  require(decoded.frames.size() == 1 && decoded.frames[0].pixels.size() == 2 &&
              decoded.frames[0].pixels[1].b == 6 && decoded.frames[0].keyframe &&
              decoded.frames[0].frame_id == "frame_000002" &&
              decoded.skipped_reason == "one miss" && decoded.frames_missed == 1,
          "canonical frames round-trip");
}

void test_journal_deleted_only_after_validated_publication() {
  const auto package_plan =
      svp::builder::execution_plan_for_stage(svp::builder::BuildStage::package_skeleton);
  svp::builder::PackageSkeletonStageResult package;
  package.package_written = true;
  package.validator_passes = true;
  require(!unfinished_publication(package_plan, package, std::nullopt),
          "a written, validated package lets the journal go");
  package.validator_passes = false;
  require(unfinished_publication(package_plan, package, std::nullopt).has_value(),
          "a package that fails strict validation keeps the journal");
  package.package_written = false;
  require(unfinished_publication(package_plan, package, std::nullopt).has_value(),
          "an unwritten package keeps the journal");

  svp::builder::SvpiPublicationResult svpi{.success = true, .validator_passed = false};
  package.package_written = true;
  package.validator_passes = true;
  require(unfinished_publication(package_plan, package, svpi).has_value(),
          "an SVPI that fails strict validation keeps the journal");
  svpi.validator_passed = true;
  require(!unfinished_publication(package_plan, package, svpi),
          "a written, validated SVPI lets the journal go");
  svpi.success = false;
  require(unfinished_publication(package_plan, package, svpi).has_value(),
          "an unwritten SVPI keeps the journal");

  const auto ingest_plan =
      svp::builder::execution_plan_for_stage(svp::builder::BuildStage::media_ingest);
  require(!unfinished_publication(ingest_plan, {}, std::nullopt),
          "--stop-after builds publish no package");
  require(kept_journal_note("x", "/tmp/out.svp-journal").find("--fresh") != std::string::npos,
          "the kept-journal note names --resume and --fresh");
}

}  // namespace

int main() {
  test_package_plan_overlaps_lanes();
  test_serial_plan_is_one_chain();
  test_single_heavy_lane_runs_audio_first();
  test_microphone_mode_orders_processor_writers();
  test_svpi_and_stop_after_plans();
  test_graph_cache_keys_follow_build_inputs();
  test_staging_capture_and_restore();
  test_stage_products_round_trip();
  test_frame_catalog_deltas_replay_in_any_order();
  test_canonical_frames_round_trip();
  test_journal_deleted_only_after_validated_publication();
  if (g_failures != 0) {
    std::cerr << g_failures << " task engine check(s) failed\n";
    return 1;
  }
  std::cout << "svp-builder task engine tests passed\n";
  return 0;
}
