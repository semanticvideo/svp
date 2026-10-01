#include "svp/exec/worker/worker_connection.hpp"

namespace svp::exec::worker {

WorkerConnection::~WorkerConnection() {
  writer.reset();
  reader.reset();
  if (connection.stream) {
    connection.stream->cancel();
  }
}

std::unique_ptr<WorkerConnection> connect_to_worker(const remote::PairingKey& key,
                                                    const remote::RoutePolicy& routes) {
  remote::RemoteConnector connector(remote::RemoteConnectorOptions{.pairing = key, .routes = routes});
  auto result = std::make_unique<WorkerConnection>();
  result->connection = connector.connect();
  result->reader = std::make_unique<StreamFrameReader>(*result->connection.stream);
  result->writer = std::make_unique<StreamFrameWriter>(*result->connection.stream);
  return result;
}

}  // namespace svp::exec::worker
