#include "svp/exec/worker/pairings_server.hpp"

#include "svp/exec/remote/transport_policy.hpp"
#include "svp/exec/worker/pairing_proof.hpp"
#include "svp/exec/worker/worker_error.hpp"

namespace svp::exec::worker {

namespace remote = svp::exec::remote;

PairingsServer::PairingsServer(PairingsServerOptions options) : options_(std::move(options)) {}

PairingsServer::~PairingsServer() { stop(); }

std::string PairingsServer::resolve(const CoordinatorHello& hello,
                                    const std::vector<std::byte>& exporter) const {
  if (!hello.pairing) {
    return {};
  }
  remote::PairingKey key;
  {
    const std::lock_guard lock(mutex_);
    const auto found = keys_.find(hello.pairing->pairing_id);
    if (found == keys_.end()) {
      throw WorkerError(WorkerErrorCode::refused,
                        "HELLO names pairing `" + hello.pairing->pairing_id +
                            "`, which this worker does not serve");
    }
    key = found->second;
  }
  if (!verify_pairing_proof(*hello.pairing, exporter, key)) {
    throw WorkerError(WorkerErrorCode::refused,
                      "HELLO's proof for pairing `" + key.pairing_id + "` does not verify");
  }
  return key.pairing_id;
}

void PairingsServer::serve(remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
  const std::vector<std::byte> exporter =
      stream.export_keying_material(kPairingProofExporterLabel, kPairingProofExporterBytes)
          .value_or(std::vector<std::byte>{});
  const CoordinatorResolver resolver = [this, exporter](const CoordinatorHello& hello) {
    return resolve(hello, exporter);
  };
  options_.serve(stream, info, resolver);
}

void PairingsServer::advertise_all(std::uint16_t port) {
  own_advertisement_.reset();
  pairing_advertisements_.clear();
  if (port == 0) {
    return;
  }
  own_advertisement_ = std::make_unique<remote::ServiceAdvertiser>(
      remote::ServiceAdvertisement{
          .name = options_.service_name,
          .port = port,
          .txt = {{std::string(remote::kWorkerTxtKey), options_.worker_id}}},
      options_.log);
}

void PairingsServer::set_pairings(const std::vector<remote::PairingKey>& pairings) {
  const std::lock_guard changing(changing_);
  std::map<std::string, remote::PairingKey> wanted;
  for (const remote::PairingKey& key : pairings) {
    remote::validate_pairing_key(key);
    wanted[key.pairing_id] = key;
  }
  bool same = false;
  {
    const std::lock_guard lock(mutex_);
    same = wanted.size() == keys_.size() &&
           std::equal(wanted.begin(), wanted.end(), keys_.begin(), [](const auto& a, const auto& b) {
             return a.first == b.first && a.second.secret == b.second.secret;
           });
  }
  if (same && (listener_ || wanted.empty())) {
    return;
  }
  if (wanted.empty()) {
    pairing_advertisements_.clear();
    own_advertisement_.reset();
    if (listener_) {
      listener_->stop();
      listener_.reset();
    }
    const std::lock_guard lock(mutex_);
    keys_.clear();
    return;
  }
  std::vector<remote::PairingKey> accepted;
  for (const auto& [id, key] : wanted) {
    accepted.push_back(key);
  }
  const remote::PairingKey first = accepted.front();
  accepted.erase(accepted.begin());
  {
    // New keys are known before the listener accepts them, removed ones
    // stay until it no longer does: a session never finds its key missing.
    const std::lock_guard lock(mutex_);
    for (const auto& [id, key] : wanted) {
      keys_[id] = key;
    }
  }
  const std::uint16_t old_port = listener_ ? listener_->port() : 0;
  if (!listener_) {
    remote::RemoteListenerOptions listener_options;
    listener_options.pairing = first;
    listener_options.accepted_keys = accepted;
    listener_options.advertise = false;
    listener_options.transport = options_.transport;
    auto listener = std::make_unique<remote::RemoteListener>(
        listener_options,
        [this](remote::RemoteStream& stream, const remote::RemoteSessionInfo& info) {
          serve(stream, info);
        });
    listener->start();
    listener_ = std::move(listener);
  } else {
    listener_->replace_keys(first, accepted);
  }
  {
    const std::lock_guard lock(mutex_);
    keys_ = wanted;
  }
  const std::uint16_t port = listener_->port();
  if (port != old_port) {
    advertise_all(port);
  }
  if (!options_.advertise_each_pairing) {
    return;
  }
  for (auto iterator = pairing_advertisements_.begin();
       iterator != pairing_advertisements_.end();) {
    iterator = wanted.contains(iterator->first) ? std::next(iterator)
                                                : pairing_advertisements_.erase(iterator);
  }
  for (const auto& [id, key] : wanted) {
    if (!pairing_advertisements_.contains(id)) {
      pairing_advertisements_.emplace(
          id, std::make_unique<remote::ServiceAdvertiser>(
                  remote::ServiceAdvertisement{
                      .name = id,
                      .port = port,
                      .txt = {{std::string(remote::kPairingTxtKey), id},
                              {std::string(remote::kWorkerTxtKey), options_.worker_id}}},
                  options_.log));
    }
  }
}

std::uint16_t PairingsServer::port() const { return listener_ ? listener_->port() : 0; }

std::size_t PairingsServer::pairing_count() const {
  const std::lock_guard lock(mutex_);
  return keys_.size();
}

std::size_t PairingsServer::sessions_started() const {
  return listener_ ? listener_->sessions_started() : 0;
}

std::vector<std::string> PairingsServer::advertised_instances() const {
  std::vector<std::string> names;
  if (own_advertisement_) {
    names.push_back(own_advertisement_->advertisement().name);
  }
  for (const auto& [id, advertiser] : pairing_advertisements_) {
    names.push_back(id);
  }
  return names;
}

bool PairingsServer::wait_advertised(std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto left = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
  };
  if (own_advertisement_ && !own_advertisement_->wait_registered(left())) {
    return false;
  }
  for (const auto& [id, advertiser] : pairing_advertisements_) {
    if (!advertiser->wait_registered(left())) {
      return false;
    }
  }
  return true;
}

void PairingsServer::stop() {
  const std::lock_guard changing(changing_);
  pairing_advertisements_.clear();
  own_advertisement_.reset();
  if (listener_) {
    listener_->stop();
  }
}

}  // namespace svp::exec::worker
