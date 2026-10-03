// Every pairing on one listener (pairings_server.hpp): many pairings served
// by one TLS-PSK listener and one Bonjour instance of the worker's own; the
// session's pairing decided only by a HELLO proof that verifies; pairings
// added and removed while serving; coordinators that predate worker ids
// finding the worker by pairing id; and the worker id book that lets new
// coordinators find it by worker id.

#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/transport_policy.hpp"
#include "svp/exec/worker/fleet_crypto.hpp"
#include "svp/exec/worker/fleet_keys.hpp"
#include "svp/exec/worker/pairing_proof.hpp"
#include "svp/exec/worker/pairings_server.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/exec/worker/worker_id_book.hpp"
#include "svp/exec/worker/worker_identity.hpp"
#include "worker_test_support.hpp"

#include <set>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
namespace remote = svp::exec::remote;
namespace fs = std::filesystem;

// Bonjour registration of a few instances: three probes 250 ms apart and an
// announcement each (RFC 6762 §8), with margin for a busy network.
constexpr std::chrono::seconds kRegistrationWait{20};

remote::PairingKey random_key() {
  return remote::PairingKey{.pairing_id = random_fleet_identifier("svpw-"),
                            .secret = secure_random_bytes(remote::kMinPairingSecretBytes)};
}

CoordinatorHello sample_hello() {
  CoordinatorHello hello;
  hello.host = detect_host_facts();
  hello.thread_plan = nlohmann::json::object();
  return hello;
}

// Worker side: answers HELLO with the coordinator the resolver decided (or
// the refusal) in an ERROR-typed frame's message, then ends.
struct Harness {
  std::string worker_id = random_fleet_identifier(kWorkerIdPrefix);
  std::mutex mutex;
  std::vector<std::string> resolved;
  PairingsServer server{PairingsServerOptions{
      .worker_id = worker_id,
      .serve =
          [this](remote::RemoteStream& stream, const remote::RemoteSessionInfo&,
                 const CoordinatorResolver& resolve) {
            svp::exec::StreamFrameReader reader(stream);
            svp::exec::StreamFrameWriter writer(stream);
            const std::optional<svp::exec::Frame> frame = reader.read();
            if (!frame) {
              return;
            }
            std::string answer;
            try {
              answer = "coordinator=" + resolve(hello_from_frame(*frame));
            } catch (const WorkerError& error) {
              answer = std::string("refused: ") + error.what();
            }
            {
              const std::lock_guard lock(mutex);
              resolved.push_back(answer);
            }
            writer.write(svp::exec::Frame{.type = svp::exec::MessageType::error,
                                          .body = {{"message", answer}},
                                          .payloads = {}});
          },
      .log = {},
      .advertise_each_pairing = true,
      // Unique per run, so parallel runs never rename each other.
      .service_name = worker_id,
      .transport = {}}};
};

// Coordinator side: connects (by worker id when given) and sends HELLO with
// the given proof; returns the worker's answer.
std::string session(const remote::PairingKey& key, const std::string& worker_id,
                    std::optional<PairingProof> proof_override = std::nullopt,
                    bool send_proof = true) {
  remote::RemoteConnector connector(
      remote::RemoteConnectorOptions{.pairing = key, .worker_id = worker_id});
  remote::RemoteConnection connection = connector.connect();
  svp::exec::StreamFrameReader reader(*connection.stream);
  svp::exec::StreamFrameWriter stream_writer(*connection.stream);
  std::optional<PairingProof> proof =
      send_proof ? (proof_override ? proof_override : prove_pairing(*connection.stream, key))
                 : std::nullopt;
  ProvingFrameWriter writer(stream_writer, proof);
  writer.write(make_hello_frame(sample_hello()));
  const std::optional<svp::exec::Frame> answer = reader.read();
  return answer ? answer->body.value("message", std::string()) : std::string("no answer");
}

template <typename Function>
void expect_transport_error(remote::RemoteErrorCode code, Function&& function,
                            std::string_view what) {
  try {
    function();
  } catch (const remote::RemoteTransportError& error) {
    expect(error.code() == code, std::string(what) + ": " + error.what());
    return;
  }
  throw std::runtime_error(std::string(what) + ": no RemoteTransportError");
}

void test_many_pairings_one_listener() {
  Harness harness;
  std::vector<remote::PairingKey> keys;
  for (int index = 0; index < 12; ++index) {
    keys.push_back(random_key());
  }
  harness.server.set_pairings(keys);
  expect(harness.server.pairing_count() == keys.size(), "every pairing is served");
  const std::uint16_t port = harness.server.port();
  expect(port != 0, "one listener");
  expect(harness.server.advertised_instances().size() == keys.size() + 1,
         "the worker's own instance plus one per pairing for older coordinators");
  expect(harness.server.wait_advertised(kRegistrationWait), "every instance registered");

  // New coordinators: by worker id, each proving its own pairing.
  for (const remote::PairingKey& key : {keys.front(), keys[5], keys.back()}) {
    expect_equal(session(key, harness.worker_id), "coordinator=" + key.pairing_id,
                 "a proven session serves its own pairing");
  }
  // Older coordinators: by pairing id, without a proof.
  expect_equal(session(keys[7], {}, std::nullopt, /*send_proof=*/false),
               std::string("coordinator="),
               "an older coordinator finds the worker by pairing id and is served unnamed");
  expect(harness.server.port() == port, "all of it on the one port");
}

void test_proofs_cannot_be_forged() {
  Harness harness;
  const remote::PairingKey a = random_key();
  const remote::PairingKey b = random_key();
  harness.server.set_pairings({a, b});
  // A coordinator holding pairing A claims pairing B.
  const PairingProof claim{.pairing_id = b.pairing_id,
                           .proof = std::string(2 * kPairingProofExporterBytes, '0')};
  const std::string forged = session(a, harness.worker_id, claim);
  expect(forged.starts_with("refused:"), "a proof that does not verify is refused: " + forged);
  // A proof made for another connection (replayed) does not verify either.
  const std::vector<std::byte> other_connection(kPairingProofExporterBytes, std::byte{7});
  const std::string replayed =
      session(a, harness.worker_id, make_pairing_proof(a, other_connection));
  expect(replayed.starts_with("refused:"), "a replayed proof is refused: " + replayed);
  // A pairing the worker does not serve.
  const remote::PairingKey stranger = random_key();
  const PairingProof unknown{.pairing_id = stranger.pairing_id, .proof = claim.proof};
  expect(session(a, harness.worker_id, unknown).starts_with("refused:"),
         "a pairing the worker does not serve is refused");
}

void test_pairings_change_while_serving() {
  Harness harness;
  const remote::PairingKey first = random_key();
  const remote::PairingKey added = random_key();
  harness.server.set_pairings({first});
  const std::uint16_t port = harness.server.port();
  expect_equal(session(first, harness.worker_id), "coordinator=" + first.pairing_id,
               "the first pairing");

  harness.server.set_pairings({first, added});
  expect(harness.server.port() == port, "the same port after a pairing was added");
  expect(harness.server.advertised_instances().size() == 3, "an instance for the new pairing");
  expect_equal(session(added, harness.worker_id), "coordinator=" + added.pairing_id,
               "a pairing added while serving works at once");
  expect(harness.server.wait_advertised(kRegistrationWait), "its instance registered");
  expect_equal(session(added, {}, std::nullopt, false), std::string("coordinator="),
               "and older coordinators find it by its pairing id");

  harness.server.set_pairings({added});
  expect(harness.server.advertised_instances().size() == 2, "the removed pairing's instance goes");
  expect_transport_error(remote::RemoteErrorCode::authentication_failed,
                         [&] { (void)session(first, harness.worker_id); },
                         "a removed pairing is refused");
  expect_equal(session(added, harness.worker_id), "coordinator=" + added.pairing_id,
               "the remaining pairing still works");

  harness.server.set_pairings({});
  expect(harness.server.port() == 0 && harness.server.advertised_instances().empty(),
         "no pairing: neither listening nor advertising");
}

void test_worker_identity_and_book() {
  TemporaryDirectory scratch("svp-worker-identity");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  const std::string id = load_or_create_worker_id(layout);
  expect(is_fleet_identifier(id, kWorkerIdPrefix), "worker id form: " + id);
  expect_equal(load_or_create_worker_id(layout), id, "the id is kept");

  const PairingDirectory store(scratch.path / "Pairings");
  CoordinatorPairingRecord record;
  record.key = random_key();
  record.created_at = "2026-10-03T00:00:00Z";
  record.runtime_id = svp::exec::blake3_digest(std::string_view("runtime"));
  record.worker.ssh_target = "w@host";
  store.write(record.key.pairing_id, encode_coordinator_pairing(record));

  WorkerIdBook book(store);
  expect(book.worker_id_of(record.key.pairing_id).empty(), "an old record has no worker id");
  book.learn(record.key.pairing_id, id);
  expect_equal(book.worker_id_of(record.key.pairing_id), id, "learned");
  const CoordinatorPairingRecord stored =
      decode_coordinator_pairing(*store.read(record.key.pairing_id));
  expect_equal(stored.worker.worker_id, id, "and written to the pairing record");
  expect_equal(stored.worker.ssh_target, std::string("w@host"), "the rest of the record is kept");
  expect_equal(WorkerIdBook(store).worker_id_of(record.key.pairing_id), id,
               "a later process reads it from the record");
}

}  // namespace

int main() {
  return run_tests("svp-exec-worker-pairings-server-tests",
                   {
                       {"many pairings, one listener", test_many_pairings_one_listener},
                       {"proofs cannot be forged", test_proofs_cannot_be_forged},
                       {"pairings change while serving", test_pairings_change_while_serving},
                       {"worker identity and book", test_worker_identity_and_book},
                   });
}
