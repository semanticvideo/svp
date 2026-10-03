#pragma once

// The coordinator's map from its pairings to their workers' ids
// (worker_identity.hpp): kept in each coordinator pairing record
// (pairing_store.hpp WorkerEndpoint::worker_id), learned from the HELLO_ACK
// of every session (AckObservingFrameReader, pairing_proof.hpp) and written
// back to the record when it changes, so discovery finds a worker by its own
// Bonjour instance (RemoteConnectorOptions::worker_id) from then on.
// Thread-safe.

#include "svp/exec/worker/pairing_store.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace svp::exec::worker {

class WorkerIdBook {
 public:
  explicit WorkerIdBook(PairingDirectory directory);

  // "" when not known (yet).
  [[nodiscard]] std::string worker_id_of(const std::string& pairing_id);
  // Remembers it, and rewrites the pairing's record when it holds another
  // id. A pairing without a record is remembered for this process only.
  void learn(const std::string& pairing_id, const std::string& worker_id);

 private:
  void load_once();

  PairingDirectory directory_;
  std::mutex mutex_;
  bool loaded_ = false;
  std::map<std::string, std::string> ids_;
};

// Over default_coordinator_pairings_dir().
[[nodiscard]] WorkerIdBook& default_worker_id_book();

}  // namespace svp::exec::worker
