#include "svp/exec/worker/worker_id_book.hpp"

namespace svp::exec::worker {

WorkerIdBook::WorkerIdBook(PairingDirectory directory) : directory_(std::move(directory)) {}

void WorkerIdBook::load_once() {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  try {
    for (const CoordinatorPairingRecord& record : load_coordinator_pairings(directory_)) {
      if (!record.worker.worker_id.empty()) {
        ids_[record.key.pairing_id] = record.worker.worker_id;
      }
    }
  } catch (const std::exception&) {
    // An unreadable store finds workers by pairing id, as before.
  }
}

std::string WorkerIdBook::worker_id_of(const std::string& pairing_id) {
  const std::lock_guard lock(mutex_);
  load_once();
  const auto found = ids_.find(pairing_id);
  return found == ids_.end() ? std::string() : found->second;
}

void WorkerIdBook::learn(const std::string& pairing_id, const std::string& worker_id) {
  if (worker_id.empty()) {
    return;
  }
  const std::lock_guard lock(mutex_);
  load_once();
  if (ids_[pairing_id] == worker_id) {
    return;
  }
  ids_[pairing_id] = worker_id;
  try {
    const std::optional<std::string> bytes = directory_.read(pairing_id);
    if (!bytes) {
      return;
    }
    CoordinatorPairingRecord record = decode_coordinator_pairing(*bytes);
    if (record.worker.worker_id != worker_id) {
      record.worker.worker_id = worker_id;
      directory_.write(pairing_id, encode_coordinator_pairing(record));
    }
  } catch (const std::exception&) {
    // The record keeps what it had; this process knows the id anyway.
  }
}

WorkerIdBook& default_worker_id_book() {
  static WorkerIdBook book{PairingDirectory(default_coordinator_pairings_dir())};
  return book;
}

}  // namespace svp::exec::worker
