#pragma once

#include "svp/models/thread_plan.hpp"
#include "svp/vision/keyframe_embedding_work.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace svp::media { struct MediaIngestPlan; }

namespace svp::vision::detail {

// One shot's keyframe: the shot's first frame (RC2 §20.6 step 2), as written by
// the timeline stage to timeline/shots.jsonl and timeline/frames.jsonl.
struct ShotKeyframe {
  std::string shot_id;
  std::string frame_id;
  std::int64_t pts_us = 0;
  int analysis_width = 0;
  int analysis_height = 0;
};

// Every shot in timeline/shots.jsonl, in file order, with its first frame
// resolved through timeline/frames.jsonl. Shots whose first frame is missing
// from frames.jsonl are skipped (and counted by the caller as unembedded).
[[nodiscard]] std::vector<ShotKeyframe> load_shot_keyframes(
    const std::filesystem::path& staging_dir);

struct ShotKeyframeVector {
  std::string shot_id;
  std::vector<float> vector;
};

struct ShotKeyframeEmbeddings {
  std::string model_id;
  std::string model_bundle_id;
  std::string model_blake3;
  std::vector<ShotKeyframeVector> vectors;  // same order as the input keyframes
  std::size_t keyframes_requested = 0;
  // Why vectors could not be produced at all (empty when vectors were made).
  std::string blocker;
};

struct ShotKeyframeEmbeddingRequest {
  const svp::media::MediaIngestPlan* media_plan = nullptr;
  std::filesystem::path ffmpeg_path;
  // Resolved bundle directory of the vision model (caller looks it up).
  std::filesystem::path model_bundle_dir;
  std::string execution_provider;
  svp::models::OrtThreadCounts threads;
  std::uint32_t embedding_dim = 0;
  // Called after each keyframe with (completed, total).
  std::function<void(std::size_t, std::size_t)> on_keyframe;
  // Optional: embeds the keyframes elsewhere (dispatched_work.hpp).
  KeyframeEmbeddingDispatcher dispatcher;
};

// Decodes each keyframe at its analysis raster and embeds it with the vision
// model, verifying the model bundle first. A keyframe that fails to decode or
// embed is left out; callers report the shortfall.
[[nodiscard]] ShotKeyframeEmbeddings embed_shot_keyframes(
    const std::vector<ShotKeyframe>& keyframes,
    const ShotKeyframeEmbeddingRequest& request);

}  // namespace svp::vision::detail
