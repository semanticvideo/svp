#include "build_blob_release.hpp"

#include "stream_deadline.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/worker_connection.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;

// How long a release session may run once its connection is authenticated
// (HELLO, HELLO_ACK, BLOB_RELEASE, SHUTDOWN). Finding the worker and the TLS
// handshake are bounded already (route_policy.hpp, transport_policy.hpp);
// this bounds a worker that accepts the connection but never answers, which
// would otherwise keep the build from exiting (the fleet waits for its
// release). It is the reconnect window, the time a build already gives a
// worker that is slow to reach (remote_executor.hpp, distributed_fleet.cpp):
// a healthy HELLO_ACK is one round trip plus the worker summarising its
// cache, far inside it, and every worker is told at once, so a silent worker
// delays the build's exit by at most this long.
constexpr std::chrono::milliseconds kReleaseSessionDeadline =
    svp::exec::remote::kDefaultReconnectWindow;

std::string worker_name(const CoordinatorPairingRecord& record) {
  return record.key.pairing_id + " (" + record.worker.ssh_target + ")";
}

// HELLO, BLOB_RELEASE, SHUTDOWN over `connection`. Returns what to report.
std::string release_over(WorkerConnection& connection, const CoordinatorPairingRecord& record,
                         const CoordinatorHello& hello, const std::vector<BlobRef>& blobs) {
  WorkerSessionClient client(*connection.reader, *connection.writer);
  const WorkerHelloAck ack = client.hello(hello);
  if (!worker_accepts_blob_release(ack.protocol)) {
    client.shutdown();
    return "warning: worker " + worker_name(record) + " speaks protocol " +
           std::to_string(ack.protocol.major) + "." + std::to_string(ack.protocol.minor) +
           ", which cannot release blobs; it keeps this build's source until its cache "
           "budget evicts it";
  }
  client.release_blobs(blobs);
  client.shutdown();
  return "worker " + worker_name(record) + ": released this build's source (" +
         std::to_string(blobs.size()) + " blob(s))";
}

// One short session, cancelled if it outlasts kReleaseSessionDeadline.
std::string release_on(const CoordinatorPairingRecord& record, const CoordinatorHello& hello,
                       const std::vector<BlobRef>& blobs) {
  const std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
  const StreamDeadline deadline(*connection->connection.stream, kReleaseSessionDeadline);
  try {
    return release_over(*connection, record, hello, blobs);
  } catch (const std::exception&) {
    if (deadline.expired()) {
      throw std::runtime_error(
          "no answer within " +
          std::to_string(
              std::chrono::duration_cast<std::chrono::seconds>(kReleaseSessionDeadline).count()) +
          " s");
    }
    throw;
  }
}

}  // namespace

void BuildBlobRelease::set_hello(CoordinatorHello hello) {
  const std::lock_guard lock(mutex_);
  hello_ = std::move(hello);
}

void BuildBlobRelease::add_blob(const BlobRef& blob) {
  const std::lock_guard lock(mutex_);
  blobs_.insert(blob);
}

void BuildBlobRelease::add_worker(const CoordinatorPairingRecord& record) {
  const std::lock_guard lock(mutex_);
  const bool known = std::any_of(workers_.begin(), workers_.end(), [&](const auto& worker) {
    return worker.key.pairing_id == record.key.pairing_id;
  });
  if (!known) {
    workers_.push_back(record);
  }
}

void BuildBlobRelease::release(const std::function<void(const std::string&)>& log) {
  std::vector<CoordinatorPairingRecord> workers;
  std::vector<BlobRef> blobs;
  CoordinatorHello hello;
  {
    const std::lock_guard lock(mutex_);
    workers = std::exchange(workers_, {});
    blobs.assign(blobs_.begin(), blobs_.end());
    blobs_.clear();
    hello = hello_;
  }
  if (workers.empty() || blobs.empty()) {
    return;
  }
  std::vector<std::future<std::string>> pending;
  for (const CoordinatorPairingRecord& record : workers) {
    pending.push_back(std::async(std::launch::async, [&record, &hello, &blobs] {
      try {
        return release_on(record, hello, blobs);
      } catch (const std::exception& error) {
        return "warning: could not release this build's source on worker " +
               worker_name(record) + " (" + error.what() +
               "); it stays until that worker's cache budget evicts it";
      }
    }));
  }
  for (std::future<std::string>& result : pending) {
    log(result.get());
  }
}

}  // namespace svp::builder::workers
