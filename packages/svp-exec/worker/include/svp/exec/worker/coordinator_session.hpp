#pragma once

// The coordinator's side of the worker handshake (plan §4.3): HELLO, then
// making sure the worker holds the coordinator's runtime and any model
// bundles it needs, all over one FrameReader / FrameWriter pair. Used as the
// RemoteExecutor session preamble before leases flow, and by the
// `svp-builder workers` commands on their own connections.

#include "svp/exec/frame_limits.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/worker/blob_source.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/runtime_source.hpp"
#include "svp/models/thread_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svp::exec::worker {

// HELLO for this Mac, the runtime it will ask workers to run, and the
// thread plan its tasks will carry.
[[nodiscard]] CoordinatorHello make_coordinator_hello(
    const CoordinatorRuntime& runtime, const svp::models::ThreadPlan& thread_plan,
    std::optional<ModelSetSummary> model_set);

struct TransferStats {
  std::uint64_t blobs_sent = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t blobs_already_present = 0;
  bool runtime_pushed = false;
  std::vector<std::string> model_bundles_pushed;
};

class WorkerSessionClient {
 public:
  WorkerSessionClient(FrameReader& reader, FrameWriter& writer, FrameLimits limits = {});

  // Sends HELLO and returns the worker's HELLO_ACK. Throws
  // WorkerError(refused) with the worker's reason when it refused.
  WorkerHelloAck hello(const CoordinatorHello& hello);

  [[nodiscard]] bool runtime_present(const Blake3Digest& runtime_id);
  // Pushes the runtime unless the worker already has it, then confirms the
  // worker verified and installed it.
  void ensure_runtime(const CoordinatorRuntime& runtime, TransferStats& stats);

  [[nodiscard]] std::vector<Blake3Digest> missing_model_bundles(
      const std::vector<Blake3Digest>& bundles);
  // Pushes every bundle the worker lacks and confirms each was verified
  // against its lock entry and installed.
  void ensure_model_bundles(const std::vector<ModelBundleSource>& bundles,
                            TransferStats& stats);

  // Sends the blobs the worker lacks, in chunks of at most the frame
  // payload limit, and confirms it now holds all of them.
  void send_blobs(const std::vector<BlobSource>& blobs, TransferStats& stats);

  // BLOB_GET (protocol 1.1): fetches a blob the worker holds into
  // `destination` (created or replaced). Each chunk is verified by the frame
  // decoder, the whole file against the blob's length and BLAKE3, and only
  // then renamed into place. Throws WorkerError(configuration) when the
  // worker does not hold the blob, (verification) when the bytes do not
  // match, (protocol, io) otherwise.
  void fetch_blob(const BlobRef& blob, const std::filesystem::path& destination);

  // BLOB_RELEASE (protocol 1.2): this coordinator no longer needs `blobs`
  // on the worker, which deletes each once nothing else claims or pins it
  // (released_blobs.hpp). Not answered. Only for a worker whose HELLO_ACK
  // passes worker_accepts_blob_release; an older one ends the session.
  void release_blobs(const std::vector<BlobRef>& blobs);

  // SHUTDOWN; the worker ends the session.
  void shutdown();

 private:
  Frame expect(MessageType type);

  FrameReader& reader_;
  FrameWriter& writer_;
  FrameLimits limits_;
};

}  // namespace svp::exec::worker
