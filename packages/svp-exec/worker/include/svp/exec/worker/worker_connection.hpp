#pragma once

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/route_policy.hpp"

#include <memory>

namespace svp::exec::worker {

// One authenticated session connection to a paired worker, found by its
// worker id when known (worker_id_book.hpp), else by pairing id (plan §3.4),
// with frame reader and writer over it. The writer adds this pairing's proof
// to HELLO, and the reader learns the worker id HELLO_ACK reports
// (pairing_proof.hpp). The connection is cancelled when this object is
// destroyed.
struct WorkerConnection {
  remote::RemoteConnection connection;
  std::unique_ptr<StreamFrameReader> stream_reader;
  std::unique_ptr<StreamFrameWriter> stream_writer;
  std::unique_ptr<FrameReader> reader;
  std::unique_ptr<FrameWriter> writer;

  WorkerConnection() = default;
  WorkerConnection(const WorkerConnection&) = delete;
  WorkerConnection& operator=(const WorkerConnection&) = delete;
  ~WorkerConnection();
};

// Throws RemoteTransportError (worker_not_found, no_route,
// authentication_failed).
[[nodiscard]] std::unique_ptr<WorkerConnection> connect_to_worker(
    const remote::PairingKey& key, const remote::RoutePolicy& routes = {});

}  // namespace svp::exec::worker
