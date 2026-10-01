// Pairing material on disk (plan §3.3): generation, record codecs, and the
// 0700 / 0600 store both sides of a pairing use.

#include "svp/exec/worker/pairing_store.hpp"
#include "worker_test_support.hpp"

#include <sys/stat.h>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
namespace fs = std::filesystem;

mode_t mode_of(const fs::path& path) {
  struct stat info{};
  expect(::stat(path.c_str(), &info) == 0, "stat " + path.string());
  return info.st_mode & 0777;
}

CoordinatorPairingRecord sample_coordinator_record() {
  CoordinatorPairingRecord record;
  record.key = generate_pairing_key();
  record.created_at = "2026-10-01T12:00:00Z";
  record.worker = WorkerEndpoint{.ssh_target = "worker@example.invalid",
                                 .user = "worker",
                                 .uid = 501,
                                 .home = "/Users/worker",
                                 .root = "/Users/worker/Library/Application Support/SVP/Worker",
                                 .service_mode = WorkerServiceMode::user_agent,
                                 .label = std::string(kWorkerJobLabel),
                                 .plist = "/Users/worker/Library/LaunchAgents/org.svp.worker.plist",
                                 .arch = "arm64",
                                 .os = OsIdentity{.product_version = "27.0.1", .build = "26A434"}};
  record.runtime_id = svp::exec::blake3_digest(std::string_view("runtime"));
  record.runtime_kind = RuntimeKind::builder_only;
  return record;
}

void test_generated_keys_are_random_and_well_formed() {
  const auto first = generate_pairing_key();
  const auto second = generate_pairing_key();
  expect(first.pairing_id.starts_with(kPairingIdPrefix), "pairing id prefix");
  expect(first.pairing_id.size() == kPairingIdPrefix.size() + 2 * kPairingIdRandomBytes,
         "pairing id length");
  expect(first.secret.size() == svp::exec::remote::kMinPairingSecretBytes, "256-bit secret");
  expect(first.pairing_id != second.pairing_id, "pairing ids differ");
  expect(first.secret != second.secret, "secrets differ");
}

void test_worker_record_round_trips() {
  const WorkerPairingRecord record{.key = generate_pairing_key(),
                                   .created_at = "2026-10-01T12:00:00Z"};
  const std::string bytes = encode_worker_pairing(record);
  const WorkerPairingRecord decoded = decode_worker_pairing(bytes);
  expect(decoded.key.pairing_id == record.key.pairing_id, "pairing id survives");
  expect(decoded.key.secret == record.key.secret, "secret survives");
  expect(decoded.created_at == record.created_at, "timestamp survives");
  expect(bytes.find(secret_hex(record.key.secret)) != std::string::npos, "secret stored as hex");
  expect_equal(encode_worker_pairing(decoded), bytes, "encoding is stable");
}

void test_coordinator_record_round_trips() {
  const CoordinatorPairingRecord record = sample_coordinator_record();
  const CoordinatorPairingRecord decoded =
      decode_coordinator_pairing(encode_coordinator_pairing(record));
  expect(decoded.key.secret == record.key.secret, "secret survives");
  expect(decoded.worker == record.worker, "worker endpoint survives");
  expect(decoded.runtime_id == record.runtime_id, "runtime id survives");
  expect(decoded.runtime_kind == RuntimeKind::builder_only, "runtime kind survives");
}

void test_decoding_rejects_the_wrong_role_and_short_secrets() {
  const WorkerPairingRecord worker{.key = generate_pairing_key(), .created_at = "t"};
  bool rejected = false;
  try {
    (void)decode_coordinator_pairing(encode_worker_pairing(worker));
  } catch (const std::exception&) {
    rejected = true;
  }
  expect(rejected, "a worker record is not a coordinator record");

  std::string bytes = encode_worker_pairing(worker);
  const std::string hex = secret_hex(worker.key.secret);
  bytes.replace(bytes.find(hex), hex.size(), hex.substr(0, 32));
  rejected = false;
  try {
    (void)decode_worker_pairing(bytes);
  } catch (const std::exception&) {
    rejected = true;
  }
  expect(rejected, "a 128-bit secret is refused");
}

void test_store_writes_private_files() {
  TemporaryDirectory scratch("svp-pairing-store");
  const PairingDirectory store(scratch.path / "Pairings");
  const CoordinatorPairingRecord record = sample_coordinator_record();
  store.write(record.key.pairing_id, encode_coordinator_pairing(record));
  expect(mode_of(store.path()) == 0700, "directory is 0700");
  expect(mode_of(store.file_for(record.key.pairing_id)) == 0600, "record is 0600");
  const auto loaded = load_coordinator_pairings(store);
  expect(loaded.size() == 1 && loaded.front().key.secret == record.key.secret, "record loads");
  expect(store.remove(record.key.pairing_id), "record removed");
  expect(load_coordinator_pairings(store).empty(), "store is empty after removal");
  expect(!store.remove(record.key.pairing_id), "second removal finds nothing");
}

void test_store_refuses_records_other_users_can_read() {
  TemporaryDirectory scratch("svp-pairing-store");
  const PairingDirectory store(scratch.path / "Pairings");
  const WorkerPairingRecord record{.key = generate_pairing_key(), .created_at = "t"};
  store.write(record.key.pairing_id, encode_worker_pairing(record));
  ::chmod(store.file_for(record.key.pairing_id).c_str(), 0644);
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)load_worker_pairings(store); },
                      "a group/world-readable record is refused");
  ::chmod(store.file_for(record.key.pairing_id).c_str(), 0600);
  ::chmod(store.path().c_str(), 0755);
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)load_worker_pairings(store); },
                      "a world-readable directory is refused");
}

void test_store_lists_records_in_id_order() {
  TemporaryDirectory scratch("svp-pairing-store");
  const PairingDirectory store(scratch.path / "pairings");
  std::vector<std::string> ids;
  for (int index = 0; index < 3; ++index) {
    const WorkerPairingRecord record{.key = generate_pairing_key(), .created_at = "t"};
    ids.push_back(record.key.pairing_id);
    store.write(record.key.pairing_id, encode_worker_pairing(record));
  }
  std::sort(ids.begin(), ids.end());
  const auto loaded = load_worker_pairings(store);
  expect(loaded.size() == 3, "three records");
  for (std::size_t index = 0; index < ids.size(); ++index) {
    expect(loaded[index].key.pairing_id == ids[index], "records come back in id order");
  }
  expect(load_worker_pairings(PairingDirectory(scratch.path / "missing")).empty(),
         "a missing directory has no records");
}

void test_unsafe_pairing_ids_never_name_files() {
  const PairingDirectory store("/nonexistent");
  expect_worker_error(WorkerErrorCode::configuration,
                      [&] { (void)store.file_for("../escape"); }, "path traversal refused");
}

}  // namespace

int main() {
  return run_tests("svp-exec-worker-pairing-store-tests",
                   {
                       {"generated keys are random and well formed",
                        test_generated_keys_are_random_and_well_formed},
                       {"worker record round trips", test_worker_record_round_trips},
                       {"coordinator record round trips", test_coordinator_record_round_trips},
                       {"decoding rejects the wrong role and short secrets",
                        test_decoding_rejects_the_wrong_role_and_short_secrets},
                       {"store writes private files", test_store_writes_private_files},
                       {"store refuses records other users can read",
                        test_store_refuses_records_other_users_can_read},
                       {"store lists records in id order", test_store_lists_records_in_id_order},
                       {"unsafe pairing ids never name files",
                        test_unsafe_pairing_ids_never_name_files},
                   });
}
