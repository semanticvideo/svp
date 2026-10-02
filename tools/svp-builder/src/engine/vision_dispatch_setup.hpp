#pragma once

// What a --distributed build needs to run the per-item work of its vision
// stages as tasks (plan §4.2, M4; svp/vision/dispatched_work.hpp): evidence
// crops (ocr.crop_batch), text and shot-keyframe embeddings
// (embed.text_batch, embed.keyframe_batch), and depth (depth.frame_batch).
//
// Unlike OCR frame batches, these items are only known once the stage that
// owns them runs (observations come out of OCR reconciliation, shots out of
// the color stage, canonical frames out of their decode), so each stage
// plans its tasks when it reaches that work and runs them to completion
// before it goes on (vision_work_dispatch.hpp). The stage timeline is
// unchanged: the work stays inside its stage.
//
// A build that is not --distributed has no setup, and its stages never
// dispatch anything.

#include "svp/builder/distributed_execution.hpp"
#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_spec.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder::engine {

struct VisionDispatchSetup {
  std::string build_session_id;
  // The build's source (role kOcrFrameBatchSourceRole), resolvable by every
  // executor.
  svp::exec::ArtifactRef source;
  // ffmpeg_build_identity() of this Mac's ffmpeg.
  std::string ffmpeg_build;
  // Every model bundle the workers were given; a stage whose model is not
  // among them does its work itself.
  std::vector<svp::exec::TaskModelRef> model_refs;
  // The task types this build dispatches, by name.
  std::map<std::string, DispatchedTypeCapacity, std::less<>> capacity;
  // Null when no worker is ready: the tasks run on this Mac only.
  std::shared_ptr<DispatchedWorkerExecutors> workers;
  // The build's Ctrl-C / SIGTERM token; null when the caller has none.
  const svp::exec::CancellationToken* cancellation = nullptr;
  // Print one summary line per dispatched stage (stderr).
  bool report = false;
  // Frees the models this Mac's slots keep loaded between tasks; called once
  // each stage's tasks are done. May be empty.
  std::function<void()> release_idle_models;
};

}  // namespace svp::builder::engine
