#include "embedding_generation/shot_keyframe_embedding.hpp"

#include "svp/media/media_ingest_plan.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using svp::vision::detail::ShotKeyframe;
using svp::vision::detail::ShotKeyframeEmbeddingRequest;
using svp::vision::detail::embed_shot_keyframes;
using svp::vision::detail::load_shot_keyframes;

namespace {

fs::path make_staging(const std::string& name) {
  const fs::path root = fs::temp_directory_path() / name;
  fs::remove_all(root);
  fs::create_directories(root / "timeline");
  return root;
}

void write_file(const fs::path& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
}

// Every shot resolves to its first frame's timestamp and analysis raster;
// shots whose first frame is missing or has no raster size are skipped.
void test_load_shot_keyframes_resolves_first_frames() {
  const fs::path staging = make_staging("svp-shot-keyframe-load");
  write_file(staging / "timeline" / "frames.jsonl",
             R"({"id":"frame_000001","pts_us":0,"analysis_width":640,"analysis_height":360})" "\n"
             R"({"id":"frame_000002","pts_us":500000,"analysis_width":320,"analysis_height":240})" "\n"
             R"({"id":"frame_000003","pts_us":900000})" "\n");
  write_file(staging / "timeline" / "shots.jsonl",
             R"({"id":"shot_000001","start_frame_id":"frame_000001"})" "\n"
             R"({"id":"shot_000002","start_frame_id":"frame_000002"})" "\n"
             R"({"id":"shot_000003","start_frame_id":"frame_missing"})" "\n"
             R"({"id":"shot_000004","start_frame_id":"frame_000003"})" "\n");

  const auto keyframes = load_shot_keyframes(staging);
  assert(keyframes.size() == 2);
  assert(keyframes[0].shot_id == "shot_000001");
  assert(keyframes[0].frame_id == "frame_000001");
  assert(keyframes[0].pts_us == 0);
  assert(keyframes[0].analysis_width == 640);
  assert(keyframes[0].analysis_height == 360);
  assert(keyframes[1].shot_id == "shot_000002");
  assert(keyframes[1].pts_us == 500000);
  assert(keyframes[1].analysis_width == 320);
  fs::remove_all(staging);
}

void test_missing_timeline_yields_no_keyframes() {
  const fs::path staging = make_staging("svp-shot-keyframe-empty");
  assert(load_shot_keyframes(staging).empty());
  fs::remove_all(staging);
}

void test_missing_inputs_block_without_work() {
  const std::vector<ShotKeyframe> keyframes = {
      {"shot_000001", "frame_000001", 0, 64, 36}};
  ShotKeyframeEmbeddingRequest request;  // no media plan, no ffmpeg
  const auto result = embed_shot_keyframes(keyframes, request);
  assert(result.vectors.empty());
  assert(!result.blocker.empty());
  assert(result.keyframes_requested == 1);

  const auto none = embed_shot_keyframes({}, request);
  assert(none.vectors.empty());
  assert(!none.blocker.empty());
}

// A model that fails to load must stop the decode thread and report a
// blocker, whatever the number of keyframes still waiting to decode.
void test_model_load_failure_stops_decoding() {
  const fs::path root = make_staging("svp-shot-keyframe-noload");
  svp::media::MediaIngestPlan plan;
  plan.source_path = root / "missing-source.mp4";

  std::vector<ShotKeyframe> keyframes;
  for (int i = 0; i < 8; ++i) {
    keyframes.push_back({"shot_" + std::to_string(i), "frame_" + std::to_string(i),
                         static_cast<std::int64_t>(i) * 1000, 64, 36});
  }
  ShotKeyframeEmbeddingRequest request;
  request.media_plan = &plan;
  request.ffmpeg_path = root / "missing-ffmpeg";
  request.model_bundle_dir = root / "missing-bundle";
  request.embedding_dim = 768;
  std::size_t progress_calls = 0;
  request.on_keyframe = [&](std::size_t, std::size_t) { ++progress_calls; };

  const auto result = embed_shot_keyframes(keyframes, request);
  assert(result.vectors.empty());
  assert(result.blocker.find("vision model") != std::string::npos);
  assert(progress_calls == 0);
  fs::remove_all(root);
}

}  // namespace

int main() {
  test_load_shot_keyframes_resolves_first_frames();
  test_missing_timeline_yields_no_keyframes();
  test_missing_inputs_block_without_work();
  test_model_load_failure_stops_decoding();
  std::cout << "shot keyframe embedding tests passed\n";
  return 0;
}
