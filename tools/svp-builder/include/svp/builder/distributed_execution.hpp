#pragma once

// The seam between a build and the Macs it may send work to (plan §3.1,
// §7.3, M3). BuildPipeline knows nothing about pairing, discovery, or the
// network: a `--distributed` build is given a DistributedExecution, and a
// local build is given none, so it never loads pairing data or opens a
// socket (plan §7.1).
//
// prepare() runs once, after the build has planned its OCR work and before
// any task runs. It reaches the paired workers, refuses those whose runtime,
// macOS, or thread plan cannot reproduce this build's output, brings each
// accepted worker the runtime, model bundles, and source it needs, and
// returns one executor per worker that is ready, sized from that worker's
// measured capacity. Workers that cannot be used are reported on stderr and
// left out; the build then runs with the rest, or alone (one Mac is a
// complete configuration). prepare() throws DistributedPreparationError only
// when the build asked for a minimum number of workers it cannot have. A
// cancelled build (Ctrl-C) stops waiting for workers and returns no workers;
// the build then ends as cancelled.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace svp::builder {

class DistributedPreparationError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One ONNX-model task type a build may dispatch (M4): the bundle and the
// settings its stage runs the model with.
struct DistributedOnnxWork {
  svp::exec::TaskModelRef model_ref;
  svp::vision::DispatchedModel model;
};

// The vision stage work a build may dispatch besides OCR frame batches (M4,
// svp/vision/dispatched_work.hpp). Evidence crops (ocr.crop_batch) re-read
// with the OCR work's PP-OCR, so they need no model of their own. A type
// whose model this Mac cannot name is not dispatched; its stage does the
// work itself.
struct DistributedVisionWork {
  bool evidence_crops = false;
  std::optional<DistributedOnnxWork> text_embeddings;      // embed.text_batch
  std::optional<DistributedOnnxWork> keyframe_embeddings;  // embed.keyframe_batch
  std::optional<DistributedOnnxWork> depth;                // depth.frame_batch
  std::uint32_t embedding_dim = 0;
};

// What workers need to run this build's ocr.frame_batch tasks.
struct DistributedOcrWork {
  std::string build_session_id;
  svp::exec::ArtifactRef source;
  std::filesystem::path source_path;
  // The PP-OCR bundles the tasks load (TaskSpec model_refs).
  std::vector<svp::exec::TaskModelRef> model_refs;
  // The options every batch carries; this Mac's model cache root.
  svp::vision::PpOcrOptions pp_ocr;
  std::filesystem::path model_cache_root;
  std::filesystem::path ffmpeg_path;
  std::string ffmpeg_build;
  // The build's thread plan; sent in HELLO, refused by workers when it is
  // not host-independent.
  svp::models::ThreadPlan thread_plan;
  // The build's Ctrl-C / SIGTERM token; null when the caller has none.
  const svp::exec::CancellationToken* cancellation = nullptr;
  // The vision stage work the build may also dispatch.
  DistributedVisionWork vision;
};

// How one dispatched task type runs on this Mac, from its measured capacity
// (plan §3.5): concurrent tasks, and seconds one item takes on one of them
// (batch sizing).
struct DispatchedTypeCapacity {
  std::size_t coordinator_slots = 0;
  double seconds_per_item = 0.0;
};

// The paired workers' executors for one dispatched task type. Each call
// returns fresh executors (one per ready worker that measured a capacity for
// the type, sized from it), to be started and stopped by one scheduler run.
class DispatchedWorkerExecutors {
 public:
  virtual ~DispatchedWorkerExecutors() = default;
  [[nodiscard]] virtual std::vector<std::unique_ptr<svp::exec::Executor>> make(
      std::string_view task_type) = 0;
};

struct DistributedFleet {
  // One per ready worker; owned by the DistributedExecution and valid until
  // it is destroyed. Each accepts only ocr.frame_batch tasks.
  std::vector<svp::exec::Executor*> workers;
  // ocr.frame_batch tasks this Mac runs at once, from its measured capacity.
  std::size_t coordinator_ocr_slots = 0;
  // Measured seconds one ocr.frame_batch sample takes on one of this Mac's
  // slots (batch sizing). Empty when not measured.
  std::optional<double> seconds_per_sample;
  // The dispatched vision task types this Mac measured a capacity for, by
  // type name; a type missing here is not dispatched.
  std::map<std::string, DispatchedTypeCapacity, std::less<>> dispatched_capacity;
  // The workers' executors for those types; null when no worker is ready.
  std::shared_ptr<DispatchedWorkerExecutors> dispatched_workers;
};

class DistributedExecution {
 public:
  virtual ~DistributedExecution() = default;
  [[nodiscard]] virtual DistributedFleet prepare(const DistributedOcrWork& work) = 0;
};

}  // namespace svp::builder
