#pragma once

// Whole-video jobs on paired Macs (M6, video_build_parameters.hpp): the
// coordinator side. A batch names, with --coordinators, which of this Mac's
// paired workers may coordinate videos; each becomes a PairedVideoBuilder.
// For one video it:
//   1. reaches the Mac by pairing id and sends HELLO declaring one
//      video.build slot, this runtime, and the batch's thread plan; then the
//      runtime, every model bundle of this Mac's lock, the lock, and the
//      source (each only when that Mac lacks it, verified there by BLAKE3);
//   2. assigns the job and waits for its result, renewing on each
//      heartbeat; a Mac that stops heartbeating for a lease period is given
//      up as unavailable;
//   3. fetches the package from that Mac's cache (BLOB_GET, verified by
//      BLAKE3) into place, and writes the run report when one was asked for;
//   4. releases the source and the package on that Mac (BLOB_RELEASE,
//      worker protocol 1.2) once the package is fetched, or the source when
//      the job failed for good there.
// When a job's session leaves a runtime that Mac's service will move to
// (service_updater.hpp), the service restarts once that session ends; the
// next job first waits for it to answer on the new runtime
// (worker_restart_wait.hpp RuntimeSwitchWatch) instead of reporting the Mac
// unavailable.
// A Mac whose agent turns the job away (busy: it coordinates another video)
// is asked again later; one that cannot be reached, refuses the session, or
// cannot build this runtime's videos is unavailable for the batch.

#include "batch_dispatch.hpp"

#include "../workers/worker_supplies.hpp"

#include "svp/exec/task_spec.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_restart_wait.hpp"
#include "svp/models/thread_plan.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace svp::builder::batch {

// What every Mac of one batch is sent.
struct VideoBuildSupplies {
  std::shared_ptr<const svp::builder::workers::WorkerSupplies> base;
  std::vector<svp::exec::TaskModelRef> model_refs;
  svp::exec::worker::BlobSource model_lock;
  std::string build_session_id;
};

// This runtime, every bundle of the lock in `model_cache`, and HELLO with
// `thread_plan`. Throws (WorkerError, std::runtime_error) when this Mac
// cannot assemble them.
[[nodiscard]] std::shared_ptr<const VideoBuildSupplies> prepare_video_build_supplies(
    const std::filesystem::path& model_cache, const svp::models::ThreadPlan& thread_plan);

class PairedVideoBuilder final : public RemoteVideoBuilder {
 public:
  PairedVideoBuilder(svp::exec::worker::CoordinatorPairingRecord record,
                     std::shared_ptr<const VideoBuildSupplies> supplies);

  [[nodiscard]] std::string name() const override;
  [[nodiscard]] RemoteVideoOutcome build(const RemoteVideoRequest& request) override;

 private:
  svp::exec::worker::CoordinatorPairingRecord record_;
  std::shared_ptr<const VideoBuildSupplies> supplies_;
  std::uint64_t attempts_ = 0;
  svp::exec::worker::RuntimeSwitchWatch switch_watch_;
};

// The builders for `coordinators` (pairing ids or user@host targets of this
// Mac's pairings). Throws WorkerError(configuration) naming an entry that
// matches no pairing, or several.
[[nodiscard]] std::vector<std::shared_ptr<RemoteVideoBuilder>> paired_video_builders(
    const std::vector<std::string>& coordinators,
    const std::shared_ptr<const VideoBuildSupplies>& supplies);

}  // namespace svp::builder::batch
