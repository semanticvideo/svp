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
// when the build asked for a minimum number of workers it cannot have.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/executor.hpp"
#include "svp/exec/task_spec.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::builder {

class DistributedPreparationError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
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
};

class DistributedExecution {
 public:
  virtual ~DistributedExecution() = default;
  [[nodiscard]] virtual DistributedFleet prepare(const DistributedOcrWork& work) = 0;
};

}  // namespace svp::builder
