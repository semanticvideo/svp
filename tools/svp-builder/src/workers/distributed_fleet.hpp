#pragma once

// `svp-builder build --distributed` (plan §3.1, §3.5, §7.3, M3): the paired
// workers a build's OCR frame batches may also run on.
//
// prepare(), once per build, for every pairing this Mac holds, all at once:
//   1. reach the worker by pairing id and send HELLO with this build's
//      runtime, macOS, and thread plan; the worker refuses a different macOS
//      version or a host-dependent thread plan, and is left out;
//   2. make sure it holds this runtime, the PP-OCR bundles the batches name
//      (verified there against the model lock), and the source media as a
//      BLAKE3-verified blob;
//   3. take its OCR slots from its calibration record, measuring it first
//      when the record is missing or stale;
// while this Mac's own OCR capacity is taken (or measured) the same way.
// A worker that cannot be reached or used is reported and left out; the
// build goes on with the others, or with this Mac alone. With
// `--require-workers N`, fewer than N ready workers fails the build before
// any task runs.
//
// Each ready worker becomes one RemoteExecutor, restricted to
// ocr.frame_batch, whose sessions repeat the HELLO and supply checks (cheap
// once everything is there) before their leases are sent.

#include "ocr_calibration_runs.hpp"

#include "svp/builder/distributed_execution.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/task_type_restricted_executor.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace svp::builder::workers {

struct DistributedFleetOptions {
  // --require-workers: the build fails unless at least this many workers
  // are ready.
  std::size_t require_workers = 0;
  bool quiet = false;
};

class PairedWorkerFleet final : public DistributedExecution {
 public:
  explicit PairedWorkerFleet(DistributedFleetOptions options);
  ~PairedWorkerFleet() override;

  [[nodiscard]] DistributedFleet prepare(const DistributedOcrWork& work) override;

 private:
  DistributedFleetOptions options_;
  std::vector<std::unique_ptr<svp::exec::remote::RemoteExecutor>> remotes_;
  std::vector<std::unique_ptr<svp::exec::TaskTypeRestrictedExecutor>> restricted_;
};

}  // namespace svp::builder::workers
