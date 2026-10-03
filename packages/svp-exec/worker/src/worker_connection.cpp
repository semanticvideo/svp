#include "svp/exec/worker/worker_connection.hpp"

#include "svp/exec/worker/pairing_proof.hpp"
#include "svp/exec/worker/worker_id_book.hpp"

namespace svp::exec::worker {

WorkerConnection::~WorkerConnection() {
  writer.reset();
  reader.reset();
  stream_writer.reset();
  stream_reader.reset();
  if (connection.stream) {
    connection.stream->cancel();
  }
}

std::unique_ptr<WorkerConnection> connect_to_worker(const remote::PairingKey& key,
                                                    const remote::RoutePolicy& routes) {
  WorkerIdBook& book = default_worker_id_book();
  remote::RemoteConnector connector(remote::RemoteConnectorOptions{
      .pairing = key, .worker_id = book.worker_id_of(key.pairing_id), .routes = routes});
  auto result = std::make_unique<WorkerConnection>();
  result->connection = connector.connect();
  result->stream_reader = std::make_unique<StreamFrameReader>(*result->connection.stream);
  result->stream_writer = std::make_unique<StreamFrameWriter>(*result->connection.stream);
  result->writer = std::make_unique<ProvingFrameWriter>(
      *result->stream_writer, prove_pairing(*result->connection.stream, key));
  result->reader = std::make_unique<AckObservingFrameReader>(
      *result->stream_reader, [&book, pairing_id = key.pairing_id](const WorkerHelloAck& ack) {
        book.learn(pairing_id, ack.worker_id);
      });
  return result;
}

}  // namespace svp::exec::worker
