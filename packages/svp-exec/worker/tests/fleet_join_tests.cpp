// Fleet join without SSH (fleet_join.hpp): P-256 signing and ECDH, the keys
// derived from a fleet secret, worker and coordinator tokens, the fleet and
// join credential files, and the join exchange itself, including refusing a
// coordinator that does not hold the fleet secret.

#include "svp/exec/worker/fleet_crypto.hpp"
#include "svp/exec/worker/fleet_join.hpp"
#include "svp/exec/worker/fleet_keys.hpp"
#include "svp/exec/worker/fleet_store.hpp"
#include "svp/exec/worker/join_service.hpp"
#include "worker_test_support.hpp"

#include <sys/stat.h>

#include <future>
#include <thread>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
namespace fs = std::filesystem;

constexpr std::uint64_t kNow = 1'790'000'000;

bool same_key(const svp::exec::remote::PairingKey& a, const svp::exec::remote::PairingKey& b) {
  return a.pairing_id == b.pairing_id && a.secret == b.secret;
}

std::span<const std::byte> bytes_of(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

FleetMembership make_membership(const FleetSecret& fleet) {
  return FleetMembership{.fleet = fleet,
                         .coordinator_id = random_fleet_identifier(kCoordinatorIdPrefix),
                         .created_at = "2026-10-03T00:00:00Z"};
}

WorkerJoinCredential make_credential(const FleetSecret& fleet) {
  return new_worker_join_credential(issue_worker_token(fleet, kNow, std::chrono::hours(24)));
}

WorkerEndpoint sample_endpoint() {
  WorkerEndpoint endpoint;
  endpoint.user = "w";
  endpoint.uid = 501;
  endpoint.home = "/Users/w";
  endpoint.root = "/Library/Application Support/SVP/Worker";
  endpoint.service_mode = WorkerServiceMode::system_daemon;
  endpoint.label = std::string(kWorkerJobLabel);
  endpoint.plist = "/Library/LaunchDaemons/org.svp.worker.plist";
  return endpoint;
}

struct JoinRun {
  std::optional<WorkerJoinResult> worker;
  std::optional<CoordinatorJoinResult> coordinator;
  std::string worker_error;
  std::string coordinator_error;
  WorkerErrorCode worker_code = WorkerErrorCode::protocol;
  WorkerErrorCode coordinator_code = WorkerErrorCode::protocol;
  int persisted = 0;
};

// Runs both sides of one join over a socket pair.
JoinRun run_join(const FleetMembership& coordinator, const WorkerJoinCredential& credential,
                 std::string_view expected_join_id) {
  FrameChannel channel;
  JoinRun run;
  std::thread worker([&] {
    try {
      run.worker = serve_fleet_join(*channel.right_reader, *channel.right_writer, credential,
                                    sample_endpoint(), detect_host_facts(),
                                    [&](const WorkerJoinResult&) { ++run.persisted; });
    } catch (const WorkerError& error) {
      run.worker_error = error.what();
      run.worker_code = error.code();
    }
    channel.close_right();
  });
  try {
    run.coordinator = join_fleet_worker(*channel.left_reader, *channel.left_writer, coordinator,
                                        expected_join_id);
  } catch (const WorkerError& error) {
    run.coordinator_error = error.what();
    run.coordinator_code = error.code();
  }
  channel.close_left();
  worker.join();
  return run;
}

void test_signing_and_agreement() {
  const EcKeyPair key = generate_ec_key_pair();
  expect(key.private_key.size() == kEcPrivateKeyBytes, "private key size");
  expect(key.public_key.size() == kEcPublicKeyBytes, "public key size");
  expect(ec_public_key_of(key.private_key) == key.public_key, "public key of the private key");
  const std::vector<std::byte> signature = ec_sign(key.private_key, bytes_of("message"));
  expect(ec_verify(key.public_key, bytes_of("message"), signature), "a signature verifies");
  expect(!ec_verify(key.public_key, bytes_of("massage"), signature),
         "a changed message does not verify");
  const EcKeyPair other = generate_ec_key_pair();
  expect(!ec_verify(other.public_key, bytes_of("message"), signature),
         "another key does not verify");
  expect(!ec_verify(std::vector<std::byte>(kEcPublicKeyBytes), bytes_of("message"), signature),
         "a malformed key does not verify");
  expect(!ec_verify(key.public_key, bytes_of("message"), {}), "an empty signature fails");
  expect(ec_shared_secret(key.private_key, other.public_key) ==
             ec_shared_secret(other.private_key, key.public_key),
         "ECDH agrees on both sides");
  expect_worker_error(WorkerErrorCode::verification,
                      [&] { (void)ec_shared_secret(key.private_key, std::vector<std::byte>(3)); },
                      "ECDH with a malformed peer key");
  expect_worker_error(WorkerErrorCode::configuration,
                      [] { (void)ec_sign(std::vector<std::byte>(5), bytes_of("m")); },
                      "signing with a malformed key");
}

void test_derived_keys() {
  const FleetSecret fleet = generate_fleet_secret();
  const FleetSecret other = generate_fleet_secret();
  expect(is_fleet_identifier(fleet.fleet_id, kFleetIdPrefix), "fleet id form: " + fleet.fleet_id);
  expect(token_join_key(fleet, "aa", 1) == token_join_key(fleet, "aa", 1),
         "token keys are deterministic");
  expect(token_join_key(fleet, "aa", 1) != token_join_key(fleet, "aa", 2),
         "the expiry is part of the token key");
  expect(token_join_key(fleet, "aa", 1) != token_join_key(fleet, "ab", 1),
         "the token id is part of the token key");
  expect(token_join_key(fleet, "aa", 1) != token_join_key(other, "aa", 1),
         "the fleet secret is part of the token key");
  expect(member_key(fleet, "svpj-1") != member_key(fleet, "svpj-2"),
         "member keys are per worker");
  expect(member_key(fleet, "svpj-1") != member_key(other, "svpj-1"),
         "member keys need the fleet secret");
  expect(member_key(fleet, "svpj-1").size() == kFleetKeyBytes, "256-bit keys");
  const std::string pairing = fleet_pairing_id("svpc-a", "svpj-b");
  expect(is_fleet_identifier(pairing, kFleetPairingIdPrefix), "pairing id form: " + pairing);
  expect(pairing == fleet_pairing_id("svpc-a", "svpj-b"), "pairing ids are stable");
  expect(pairing != fleet_pairing_id("svpc-c", "svpj-b"), "pairing ids are per coordinator");
}

void test_tokens() {
  const FleetSecret fleet = generate_fleet_secret();
  const WorkerJoinToken token = issue_worker_token(fleet, kNow, kDefaultWorkerTokenLifetime);
  expect(token.expires_at == kNow + std::chrono::seconds(kDefaultWorkerTokenLifetime).count(),
         "the default lifetime");
  expect(token.join_key == token_join_key(fleet, token.token_id, token.expires_at),
         "a coordinator recomputes the token's key");
  expect(token.fleet_public_key == ec_public_key_of(fleet.signing_key),
         "the token carries the fleet public key");
  const std::string text = encode_worker_token(token);
  expect(text.starts_with(kWorkerTokenPrefix), "worker token prefix");
  expect(text.find_first_of(" '\"$`\\\n") == std::string::npos, "the token is shell-safe");
  expect(decode_worker_token(text) == token, "worker token round trip");
  expect(text.find("secret") == std::string::npos, "a worker token carries no fleet secret");

  const std::string coordinator = encode_coordinator_token(fleet);
  expect(decode_coordinator_token(coordinator) == fleet, "coordinator token round trip");
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)decode_worker_token(coordinator); },
                      "a coordinator token is not a worker token");
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)decode_coordinator_token(text); },
                      "a worker token is not a coordinator token");
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)decode_worker_token(text.substr(0, text.size() - 5)); },
                      "a truncated token");
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)decode_worker_token("svpjoin1.!!!"); }, "garbage");
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)issue_worker_token(fleet, kNow, std::chrono::seconds(0)); },
                      "a lifetime must be positive");
  expect_worker_error(
      WorkerErrorCode::configuration,
      [&] {
        (void)issue_worker_token(fleet, kNow,
                                 kMaxWorkerTokenLifetime + std::chrono::hours(1));
      },
      "a lifetime beyond the maximum");
}

void test_coordinator_join_key() {
  const FleetSecret fleet = generate_fleet_secret();
  WorkerJoinCredential credential = make_credential(fleet);
  const std::string txt = join_txt_value(credential);
  expect(txt.starts_with(kJoinTxtTokenPrefix), "a new worker advertises its token: " + txt);
  const JoinListenerKey key =
      coordinator_join_key(fleet, credential.worker_join_id, txt, kNow);
  expect(key.key && same_key(*key.key, join_listener_key(credential)),
         "the coordinator derives the worker's join listener key");
  const JoinListenerKey expired = coordinator_join_key(fleet, credential.worker_join_id, txt,
                                                       credential.token.expires_at + 1);
  expect(!expired.key && expired.refusal.find("expired") != std::string::npos,
         "an expired token is refused: " + expired.refusal);
  expect(!coordinator_join_key(fleet, credential.worker_join_id, "token.zz.1", kNow).key,
         "a malformed advertisement is refused");
  expect(!coordinator_join_key(fleet, "host-name", txt, kNow).key,
         "a malformed join id is refused");

  credential.member_key = member_key(fleet, credential.worker_join_id);
  expect_equal(join_txt_value(credential), std::string(kJoinTxtMember), "a member advertises so");
  const JoinListenerKey member = coordinator_join_key(fleet, credential.worker_join_id,
                                                      kJoinTxtMember,
                                                      credential.token.expires_at + 1);
  expect(member.key && same_key(*member.key, join_listener_key(credential)),
         "a member stays pairable after its token expired");
}

void test_stores() {
  TemporaryDirectory scratch("svp-fleet-store");
  const FleetMembership membership = make_membership(generate_fleet_secret());
  const fs::path directory = scratch.path / "Fleet";
  expect(!load_fleet_membership(directory), "no fleet yet");
  save_fleet_membership(directory, membership);
  expect(load_fleet_membership(directory) == membership, "fleet record round trip");
  struct stat info{};
  expect(::stat((directory / "fleet.json").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
         "the fleet record is 0600");
  ::chmod((directory / "fleet.json").c_str(), 0644);
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)load_fleet_membership(directory); },
                      "a fleet record others can read is refused");

  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  WorkerJoinCredential credential = make_credential(membership.fleet);
  save_worker_join_credential(layout, credential);
  expect(load_worker_join_credential(layout) == credential, "join credential round trip");
  expect(::stat(layout.join_credential().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
         "the join credential is 0600");
  credential.member_key = member_key(membership.fleet, credential.worker_join_id);
  save_worker_join_credential(layout, credential);
  expect(load_worker_join_credential(layout) == credential, "with a member key");
  expect(join_listener_key(credential).secret == *credential.member_key,
         "a member listens with its member key");
  expect(read_file(layout.join_credential()).find("fleet_secret") == std::string::npos,
         "a worker never stores the fleet secret");
}

void test_join_pairs_both_sides() {
  const FleetMembership coordinator = make_membership(generate_fleet_secret());
  const WorkerJoinCredential credential = make_credential(coordinator.fleet);
  const JoinRun run = run_join(coordinator, credential, credential.worker_join_id);
  expect(run.worker && run.coordinator,
         "both sides finish: " + run.worker_error + " / " + run.coordinator_error);
  expect(run.persisted == 1, "the worker stored the join once, before JOIN_DONE");
  const std::string pairing_id =
      fleet_pairing_id(coordinator.coordinator_id, credential.worker_join_id);
  expect_equal(run.worker->pairing.key.pairing_id, pairing_id, "worker pairing id");
  expect_equal(run.coordinator->key.pairing_id, pairing_id, "coordinator pairing id");
  expect(run.worker->pairing.key.secret == run.coordinator->key.secret,
         "both sides hold the same pairing secret");
  expect(run.worker->pairing.key.secret.size() == kFleetKeyBytes, "a 256-bit pairing secret");
  expect(run.worker->member_key == member_key(coordinator.fleet, credential.worker_join_id),
         "the worker received its member key");
  expect(run.coordinator->worker.join_id == credential.worker_join_id &&
             run.coordinator->worker.user == "w" &&
             run.coordinator->worker.service_mode == WorkerServiceMode::system_daemon &&
             run.coordinator->worker.ssh_target.empty(),
         "the coordinator records the worker's endpoint without an ssh target");

  const JoinRun again = run_join(coordinator, credential, credential.worker_join_id);
  expect(again.worker && again.coordinator, "a re-join succeeds");
  expect_equal(again.coordinator->key.pairing_id, pairing_id, "a re-join keeps the pairing id");
  expect(again.coordinator->key.secret != run.coordinator->key.secret,
         "a re-join gets a fresh secret");

  const FleetMembership second = make_membership(coordinator.fleet);
  const JoinRun other = run_join(second, credential, credential.worker_join_id);
  expect(other.coordinator && other.coordinator->key.pairing_id != pairing_id,
         "another coordinator of the fleet gets its own pairing");
}

void test_refuses_without_the_fleet_secret() {
  const FleetMembership coordinator = make_membership(generate_fleet_secret());
  const WorkerJoinCredential credential = make_credential(coordinator.fleet);

  // Someone who knows the fleet id and the token (another worker, say), but
  // not the fleet signing key.
  FleetMembership impostor = make_membership(generate_fleet_secret());
  impostor.fleet.fleet_id = coordinator.fleet.fleet_id;
  const JoinRun run = run_join(impostor, credential, credential.worker_join_id);
  expect(!run.worker && run.worker_code == WorkerErrorCode::verification,
         "the worker refuses an unsigned join: " + run.worker_error);
  expect(run.persisted == 0, "nothing is stored");
  expect(!run.coordinator, "the impostor gets no pairing");

  FleetMembership elsewhere = make_membership(generate_fleet_secret());
  const JoinRun wrong_fleet = run_join(elsewhere, credential, credential.worker_join_id);
  expect(!wrong_fleet.worker && wrong_fleet.worker_code == WorkerErrorCode::refused,
         "a coordinator of another fleet is refused: " + wrong_fleet.worker_error);
  expect(!wrong_fleet.coordinator && wrong_fleet.coordinator_code == WorkerErrorCode::refused,
         "and is told so: " + wrong_fleet.coordinator_error);

  const JoinRun wrong_worker =
      run_join(coordinator, credential, random_fleet_identifier(kWorkerJoinIdPrefix));
  expect(!wrong_worker.coordinator && wrong_worker.coordinator_code == WorkerErrorCode::verification,
         "a coordinator refuses a worker answering under another join id");
  expect(wrong_worker.persisted == 0, "and the worker stores nothing");
}

void test_join_service_stores_the_pairing() {
  TemporaryDirectory scratch("svp-join-service");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  const FleetMembership coordinator = make_membership(generate_fleet_secret());
  const WorkerJoinCredential credential = make_credential(coordinator.fleet);
  save_worker_join_credential(layout, credential);
  std::mutex mutex;

  const auto join_once = [&] {
    FrameChannel channel;
    std::promise<JoinServiceOutcome> served;
    std::thread worker([&] {
      try {
        served.set_value(
            handle_join_connection(*channel.right_reader, *channel.right_writer, layout, mutex));
      } catch (...) {
        served.set_exception(std::current_exception());
      }
      channel.close_right();
    });
    const CoordinatorJoinResult joined = join_fleet_worker(
        *channel.left_reader, *channel.left_writer, coordinator, credential.worker_join_id);
    channel.close_left();
    worker.join();
    return std::make_pair(joined, served.get_future().get());
  };

  const auto [first, first_outcome] = join_once();
  expect(first_outcome.pairing_id == first.key.pairing_id, "the outcome names the pairing");
  expect(first_outcome.listener_key_changed, "the first join replaces the token key");
  const std::optional<std::string> record =
      PairingDirectory(layout.pairings()).read(first.key.pairing_id);
  expect(record && same_key(decode_worker_pairing(*record).key, first.key),
         "the worker's pairing record holds the joined secret");
  const std::optional<WorkerJoinCredential> stored = load_worker_join_credential(layout);
  expect(stored && stored->member_key == member_key(coordinator.fleet, credential.worker_join_id),
         "the member key is stored");

  const auto [second, second_outcome] = join_once();
  expect(!second_outcome.listener_key_changed, "a second join keeps the member key");
  expect(load_worker_pairings(PairingDirectory(layout.pairings())).size() == 1,
         "a re-join replaces the pairing record");
  expect(describe_this_worker(layout).root == layout.root.string(),
         "the worker describes its own root");
}

void test_join_id_is_bound_to_the_worker_key() {
  const WorkerJoinCredential credential = make_credential(generate_fleet_secret());
  expect(credential.worker_key.size() == kEcPrivateKeyBytes, "a long-term worker key");
  expect_equal(credential.worker_join_id,
               worker_join_id_for(ec_public_key_of(credential.worker_key)),
               "the join id is derived from its public key");
  expect(is_fleet_identifier(credential.worker_join_id, kWorkerJoinIdPrefix),
         "join id form: " + credential.worker_join_id);
  expect(decode_worker_join_credential(encode_worker_join_credential(credential)) == credential,
         "the key is stored with the credential");
}

void test_a_token_holder_cannot_claim_another_workers_id() {
  const FleetMembership coordinator = make_membership(generate_fleet_secret());
  const WorkerJoinCredential victim = make_credential(coordinator.fleet);
  // The attacker holds a valid token (its own Mac's credential) and answers
  // for the victim's join id, with its own key, or with the victim's public
  // key it cannot sign for.
  WorkerJoinCredential attacker = make_credential(coordinator.fleet);
  attacker.worker_join_id = victim.worker_join_id;
  const JoinRun own_key = run_join(coordinator, attacker, victim.worker_join_id);
  expect(!own_key.coordinator && own_key.coordinator_code == WorkerErrorCode::verification,
         "an id not derived from the presented key is refused: " + own_key.coordinator_error);
  expect(own_key.persisted == 0, "the attacker received no pairing and no member key");

  // The attacker presents the victim's public key (so the id matches) but
  // can only sign with its own key.
  {
    FrameChannel channel;
    std::thread fake_worker([&] {
      try {
        (void)channel.right_reader->read();  // JOIN_OFFER
        const EcKeyPair ephemeral = generate_ec_key_pair();
        const auto hex = [](const std::vector<std::byte>& bytes) {
          static constexpr char kDigits[] = "0123456789abcdef";
          std::string text;
          for (const std::byte byte : bytes) {
            text += kDigits[static_cast<unsigned>(byte) >> 4U];
            text += kDigits[static_cast<unsigned>(byte) & 0xFU];
          }
          return text;
        };
        const std::vector<std::byte> signature =
            ec_sign(attacker.worker_key, bytes_of("anything the attacker can sign"));
        channel.right_writer->write(svp::exec::Frame{
            .type = svp::exec::MessageType::join_challenge,
            .body = {{"host", host_facts_to_json(detect_host_facts())},
                     {"worker",
                      {{"home", "/Users/w"},
                       {"label", std::string(kWorkerJobLabel)},
                       {"plist", "/Library/LaunchDaemons/org.svp.worker.plist"},
                       {"root", "/Library/Application Support/SVP/Worker"},
                       {"service_mode", "system_daemon"},
                       {"uid", 501},
                       {"user", "w"}}},
                     {"worker_ephemeral", hex(ephemeral.public_key)},
                     {"worker_join_id", victim.worker_join_id},
                     {"worker_public_key", hex(ec_public_key_of(victim.worker_key))},
                     {"worker_signature", hex(signature)}},
            .payloads = {}});
        while (channel.right_reader->read()) {
        }
      } catch (const std::exception&) {
      }
      channel.close_right();
    });
    std::string refusal;
    WorkerErrorCode code = WorkerErrorCode::protocol;
    bool paired = false;
    try {
      (void)join_fleet_worker(*channel.left_reader, *channel.left_writer, coordinator,
                              victim.worker_join_id);
      paired = true;
    } catch (const WorkerError& error) {
      refusal = error.what();
      code = error.code();
    }
    channel.close_left();
    fake_worker.join();
    expect(!paired && code == WorkerErrorCode::verification,
           "a signature not made with the id's key is refused before the member key is "
           "wrapped: " + refusal);
  }

  const JoinRun genuine = run_join(coordinator, victim, victim.worker_join_id);
  expect(genuine.coordinator && genuine.worker, "the owner of the id joins");
}

void test_legacy_credential_is_migrated() {
  const FleetSecret fleet = generate_fleet_secret();
  WorkerJoinCredential legacy = make_credential(fleet);
  legacy.worker_key.clear();
  legacy.worker_join_id = random_fleet_identifier(kWorkerJoinIdPrefix);
  legacy.member_key = member_key(fleet, legacy.worker_join_id);
  const std::string legacy_text = encode_worker_join_credential(legacy);
  expect(legacy_text.find("worker_key") == std::string::npos, "the old file format");
  WorkerJoinCredential migrated = decode_worker_join_credential(legacy_text);
  expect(migrate_join_credential(migrated), "a credential without a key is migrated");
  expect_equal(migrated.worker_join_id, worker_join_id_for(ec_public_key_of(migrated.worker_key)),
               "to a key-bound join id");
  expect(!migrated.member_key, "the member key of the unproven old id is dropped");
  expect(migrated.token == legacy.token, "the token is kept");
  expect(!migrate_join_credential(migrated), "a migrated credential stays as it is");

  TemporaryDirectory scratch("svp-join-migrate");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  save_worker_join_credential(layout, legacy);
  std::mutex mutex;
  const std::optional<WorkerJoinCredential> loaded = load_current_join_credential(layout, mutex);
  expect(loaded && !loaded->worker_key.empty(), "loading migrates");
  expect(load_worker_join_credential(layout) == loaded, "and saves the migrated credential");
}

void test_join_service_does_not_hold_the_lock_while_waiting() {
  TemporaryDirectory scratch("svp-join-lock");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  const FleetMembership coordinator = make_membership(generate_fleet_secret());
  save_worker_join_credential(layout, make_credential(coordinator.fleet));
  std::mutex mutex;
  FrameChannel channel;
  std::thread worker([&] {
    try {
      (void)handle_join_connection(*channel.right_reader, *channel.right_writer, layout, mutex);
    } catch (const std::exception&) {
      // The peer went away without a word.
    }
  });
  // The peer says nothing: the worker waits for JOIN_OFFER.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const bool free = mutex.try_lock();
  if (free) {
    mutex.unlock();
  }
  expect(free, "a join waiting on a silent peer does not hold the credential lock");
  channel.close_left();
  worker.join();
  expect(kJoinExchangeTimeout.count() > 0, "both sides bound the exchange");
}

void test_issued_member_key_is_stored_for_this_join_id() {
  TemporaryDirectory scratch("svp-join-issue");
  const WorkerLayout layout{.root = scratch.path / "Worker"};
  create_worker_layout(layout);
  const FleetSecret fleet = generate_fleet_secret();
  const WorkerJoinCredential credential = make_credential(fleet);
  save_worker_join_credential(layout, credential);
  std::mutex mutex;
  std::optional<FleetJoinState> state = current_fleet_state(layout, mutex);
  expect(state && state->join_id == credential.worker_join_id && !state->member &&
             state->fleet_id == fleet.fleet_id,
         "the worker reports it holds no member key");
  bool changed = false;
  const std::vector<std::byte> key = member_key(fleet, credential.worker_join_id);
  expect(store_issued_member_key(layout, mutex, "svpj-ffffffffffffffffffffffff", key, changed)
                 .find("join id") != std::string::npos &&
             !changed,
         "a key for another join id is refused");
  expect(store_issued_member_key(layout, mutex, credential.worker_join_id, key, changed).empty() &&
             changed,
         "its own key is stored");
  expect(load_worker_join_credential(layout)->member_key == key, "in join.json");
  expect(current_fleet_state(layout, mutex)->member, "and reported");
  expect(join_listener_key(*load_worker_join_credential(layout)).secret == key,
         "the join listener switches to member mode");
  expect(store_issued_member_key(layout, mutex, credential.worker_join_id, key, changed).empty() &&
             !changed,
         "storing it again changes nothing");
}

}  // namespace

int main() {
  return run_tests("svp-exec-worker-fleet-join-tests",
                   {
                       {"signing and agreement", test_signing_and_agreement},
                       {"derived keys", test_derived_keys},
                       {"tokens", test_tokens},
                       {"coordinator join key", test_coordinator_join_key},
                       {"stores", test_stores},
                       {"join pairs both sides", test_join_pairs_both_sides},
                       {"refuses without the fleet secret", test_refuses_without_the_fleet_secret},
                       {"join service stores the pairing", test_join_service_stores_the_pairing},
                       {"join id is bound to the worker key", test_join_id_is_bound_to_the_worker_key},
                       {"a token holder cannot claim another worker's id",
                        test_a_token_holder_cannot_claim_another_workers_id},
                       {"legacy credential is migrated", test_legacy_credential_is_migrated},
                       {"issued member key is stored for this join id",
                        test_issued_member_key_is_stored_for_this_join_id},
                       {"join service does not hold the lock while waiting",
                        test_join_service_does_not_hold_the_lock_while_waiting},
                   });
}
