#pragma once

// What a `--distributed` build sent its workers for its own use, released
// when the build ends (worker protocol 1.2, BLOB_RELEASE): the source media
// and the files its dispatched tasks read (the staged analysis audio). The
// runtime, model bundles, and calibration clips are shared by every build
// and stay cached.
//
// Every worker the build reached is told, whether the build succeeded,
// failed, or was cancelled. Each deletes the blobs once no other
// coordinator still claims them and no live session pins them
// (svp/exec/worker/released_blobs.hpp). A worker that cannot be reached,
// does not answer within a time limit, or speaks protocol 1.1 keeps them
// until its cache budget evicts them, as before. Thread-safe.

#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/transfer_messages.hpp"

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace svp::builder::workers {

class BuildBlobRelease {
 public:
  // The HELLO release sessions send (the build's own: workers refuse a
  // session whose runtime, macOS, or thread plan they would refuse anyway).
  void set_hello(svp::exec::worker::CoordinatorHello hello);
  void add_blob(const svp::exec::worker::BlobRef& blob);
  // A worker the build reached; it may hold the build's blobs.
  void add_worker(const svp::exec::worker::CoordinatorPairingRecord& record);

  // Tells every worker added to release every blob added, all workers at
  // once, then forgets both. Never throws: a worker that cannot be told is
  // reported through `log` as a warning.
  void release(const std::function<void(const std::string&)>& log);

 private:
  std::mutex mutex_;
  svp::exec::worker::CoordinatorHello hello_;
  std::set<svp::exec::worker::BlobRef> blobs_;
  std::vector<svp::exec::worker::CoordinatorPairingRecord> workers_;
};

}  // namespace svp::builder::workers
