#include "svp/exec/remote/remote_listener.hpp"

#include "nw_ref.hpp"
#include "nw_stream.hpp"
#include "route_probe_responder.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "tls_psk_parameters.hpp"

#include <algorithm>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace svp::exec::remote {

using detail::NwRef;
using detail::NwStream;

namespace {

// One nw_listener's life: a listener without advertisement is replaced by
// a new one when its keys change, and each reports its own state here.
struct Opening {
  std::mutex mutex;
  std::condition_variable changed;
  bool ready = false;
  bool cancelled = false;
  std::string failure;
  std::uint16_t port = 0;
};

}  // namespace

struct RemoteListener::Core {
  struct Session {
    std::unique_ptr<NwStream> stream;
    std::thread thread;
    bool done = false;
  };

  Core(RemoteListenerOptions options_in, RemoteSessionHandler handler_in)
      : options(std::move(options_in)), handler(std::move(handler_in)) {}

  RemoteListenerOptions options;
  RemoteSessionHandler handler;
  detail::SerialQueue queue{"org.svp.exec.remote.listener"};

  mutable std::mutex mutex;
  std::condition_variable changed;
  NwRef<nw_listener_t> listener;
  std::shared_ptr<Opening> opening;
  bool advertised = false;
  std::uint16_t port = 0;
  std::string advertised_name;
  bool started = false;
  bool stopping = false;
  std::list<std::shared_ptr<Session>> sessions;
  std::uint64_t sessions_started = 0;
  std::size_t handshakes_rejected = 0;
  std::size_t route_probes_served = 0;
  // Serializes replace_keys() calls.
  std::mutex replacing;

  // Caller holds the mutex. Joins sessions whose handler already returned.
  void reap_done_sessions() {
    for (auto iterator = sessions.begin(); iterator != sessions.end();) {
      if ((*iterator)->done) {
        (*iterator)->thread.join();
        iterator = sessions.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  static void accept(const std::shared_ptr<Core>& core, NwRef<nw_connection_t> connection) {
    const std::lock_guard lock(core->mutex);
    if (core->stopping) {
      nw_connection_cancel(connection.get());
      return;
    }
    core->reap_done_sessions();
    auto session = std::make_shared<Session>();
    session->stream = std::make_unique<NwStream>(std::move(connection));
    session->thread = std::thread([core, session] { serve(core, session); });
    core->sessions.push_back(std::move(session));
  }

  static void serve(const std::shared_ptr<Core>& core, const std::shared_ptr<Session>& session) {
    NwStream& stream = *session->stream;
    stream.start();
    const bool authenticated =
        stream.wait_ready(std::chrono::steady_clock::now() +
                          core->options.transport.handshake_timeout) &&
        detail::is_expected_tls_session(stream.tls_session());
    const std::string protocol = stream.tls_session().application_protocol;
    const bool is_probe = authenticated && protocol == kRouteProbeAlpn;
    const bool is_session = authenticated && protocol == kSessionAlpn;
    RemoteSessionInfo info;
    bool serve_it = false;
    {
      const std::lock_guard lock(core->mutex);
      if (!is_probe && !is_session) {
        ++core->handshakes_rejected;
      } else if (is_probe) {
        ++core->route_probes_served;
      } else if (!core->stopping) {
        serve_it = true;
        info.session_number = ++core->sessions_started;
        info.peer = stream.peer_description();
      }
    }
    if (is_probe) {
      (void)detail::answer_route_probe(stream);
    }
    if (serve_it) {
      try {
        core->handler(stream, info);
      } catch (...) {
        // A handler failure ends only its own session; the listener keeps
        // serving others.
      }
    }
    stream.cancel();
    const std::lock_guard lock(core->mutex);
    session->done = true;
  }
};

namespace {

void on_advertised(const std::shared_ptr<RemoteListener::Core>& core, nw_endpoint_t endpoint,
                   bool added) {
  {
    const std::lock_guard lock(core->mutex);
    if (added) {
      core->advertised = true;
      if (const char* name = nw_endpoint_get_bonjour_service_name(endpoint)) {
        core->advertised_name = name;
      }
    }
  }
  core->changed.notify_all();
}

// Creates and starts one nw_listener for `keys` on `port` (0: any). Does not
// wait. Throws RemoteTransportError(listener_failed) when the system refuses.
NwRef<nw_listener_t> open_listener(std::shared_ptr<RemoteListener::Core> core,
                                   const std::vector<PairingKey>& keys, std::uint16_t port,
                                   std::shared_ptr<Opening> opening) {
  // Held by value: blocks capture a C++ reference as a reference, which
  // would dangle once this function returns.
  const RemoteListenerOptions& options = core->options;
  const NwRef<nw_parameters_t> parameters = detail::make_tls_psk_parameters(
      keys, options.transport, {std::string(kSessionAlpn), std::string(kRouteProbeAlpn)});
  if (!options.advertise) {
    // A replacement binds the port its predecessor just released.
    nw_parameters_set_reuse_local_address(parameters.get(), true);
  }
  NwRef<nw_listener_t> listener;
  if (port == 0) {
    listener = NwRef<nw_listener_t>::adopt(nw_listener_create(parameters.get()));
  } else {
    const std::string text = std::to_string(port);
    listener =
        NwRef<nw_listener_t>::adopt(nw_listener_create_with_port(text.c_str(), parameters.get()));
  }
  if (!listener) {
    throw RemoteTransportError(RemoteErrorCode::listener_failed,
                               "the system refused to create a listener");
  }
  if (options.advertise) {
    const std::string type(kWorkerServiceType);
    auto advertise = NwRef<nw_advertise_descriptor_t>::adopt(
        nw_advertise_descriptor_create_bonjour_service(
            options.service_name.empty() ? nullptr : options.service_name.c_str(),
            type.c_str(), nullptr));
    auto txt = NwRef<nw_txt_record_t>::adopt(nw_txt_record_create_dictionary());
    const std::string key(kPairingTxtKey);
    nw_txt_record_set_key(txt.get(), key.c_str(),
                          reinterpret_cast<const std::uint8_t*>(options.pairing.pairing_id.data()),
                          options.pairing.pairing_id.size());
    for (const auto& [txt_key, txt_value] : options.txt) {
      nw_txt_record_set_key(txt.get(), txt_key.c_str(),
                            reinterpret_cast<const std::uint8_t*>(txt_value.data()),
                            txt_value.size());
    }
    nw_advertise_descriptor_set_txt_record_object(advertise.get(), txt.get());
    nw_listener_set_advertise_descriptor(listener.get(), advertise.get());
    nw_listener_set_advertised_endpoint_changed_handler(
        listener.get(), ^(nw_endpoint_t endpoint, bool added) {
          on_advertised(core, endpoint, added);
        });
  }
  nw_listener_t raw = listener.get();
  nw_listener_set_queue(raw, core->queue.get());
  nw_listener_set_state_changed_handler(raw, ^(nw_listener_state_t state, nw_error_t error) {
    {
      const std::lock_guard lock(opening->mutex);
      switch (state) {
        case nw_listener_state_ready:
          opening->ready = true;
          opening->port = nw_listener_get_port(raw);
          break;
        case nw_listener_state_failed:
          opening->failure = "listener failed: " + detail::describe_nw_error(error);
          break;
        case nw_listener_state_cancelled:
          opening->cancelled = true;
          break;
        default:
          break;
      }
    }
    opening->changed.notify_all();
    core->changed.notify_all();
  });
  nw_listener_set_new_connection_handler(raw, ^(nw_connection_t connection) {
    RemoteListener::Core::accept(core, NwRef<nw_connection_t>::retain(connection));
  });
  nw_listener_start(raw);
  return listener;
}

// Waits until `opening` is ready or failed, within `timeout`; returns the
// failure ("" when ready).
std::string wait_opened(Opening& opening, std::chrono::milliseconds timeout) {
  std::unique_lock lock(opening.mutex);
  const bool settled = opening.changed.wait_for(
      lock, timeout, [&] { return opening.ready || !opening.failure.empty(); });
  if (!opening.failure.empty()) {
    return opening.failure;
  }
  return settled ? std::string() : "listener start did not finish within the timeout";
}

}  // namespace

RemoteListener::RemoteListener(RemoteListenerOptions options, RemoteSessionHandler handler)
    : core_(std::make_shared<Core>(std::move(options), std::move(handler))) {}

RemoteListener::~RemoteListener() { stop(); }

void RemoteListener::start() {
  const std::shared_ptr<Core> core = core_;
  {
    const std::lock_guard lock(core->mutex);
    if (core->started) {
      throw RemoteTransportError(RemoteErrorCode::listener_failed, "listener already started");
    }
    core->started = true;
  }
  if (!core->handler) {
    throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                               "listener needs a session handler");
  }
  const RemoteListenerOptions& options = core->options;
  if (options.advertise) {
    for (const auto& [txt_key, txt_value] : options.txt) {
      if (txt_key.empty() || txt_key == kPairingTxtKey ||
          txt_key.find('=') != std::string::npos ||
          txt_key.size() + 1 + txt_value.size() > kMaxTxtEntryBytes) {
        throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                                   "TXT entry `" + txt_key + "` is not a valid extra entry");
      }
    }
  }
  if (options.advertise_timeout.count() <= 0) {
    throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                               "advertise_timeout must be positive");
  }
  std::vector<PairingKey> keys = options.accepted_keys;
  keys.push_back(options.pairing);
  const auto opening = std::make_shared<Opening>();
  NwRef<nw_listener_t> listener = open_listener(core, keys, options.port, opening);
  {
    const std::lock_guard lock(core->mutex);
    core->listener = listener;
    core->opening = opening;
  }
  std::string failure = wait_opened(*opening, options.advertise_timeout);
  if (failure.empty()) {
    const std::lock_guard lock(opening->mutex);
    const std::lock_guard core_lock(core->mutex);
    core->port = opening->port;
  }
  if (failure.empty() && options.advertise) {
    std::unique_lock lock(core->mutex);
    if (!core->changed.wait_for(lock, options.advertise_timeout,
                                [&] { return core->advertised; })) {
      failure = "Bonjour registration did not finish within the advertise timeout";
    }
  }
  if (!failure.empty()) {
    stop();
    throw RemoteTransportError(RemoteErrorCode::listener_failed, failure);
  }
}

void RemoteListener::replace_keys(PairingKey pairing, std::vector<PairingKey> accepted) {
  const std::shared_ptr<Core> core = core_;
  if (core->options.advertise) {
    throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                               "an advertising listener cannot change its keys");
  }
  std::vector<PairingKey> keys = accepted;
  keys.push_back(pairing);
  for (const PairingKey& key : keys) {
    validate_pairing_key(key);
  }
  const std::lock_guard replacing(core->replacing);
  NwRef<nw_listener_t> old_listener;
  std::shared_ptr<Opening> old_opening;
  std::uint16_t port = 0;
  {
    const std::lock_guard lock(core->mutex);
    if (!core->started || core->stopping) {
      throw RemoteTransportError(RemoteErrorCode::listener_failed, "listener is not serving");
    }
    old_listener = core->listener;
    old_opening = core->opening;
    port = core->port;
    core->options.pairing = pairing;
    core->options.accepted_keys = accepted;
  }
  // Releasing the port first: two sockets listening on it at once would
  // split connections between the old keys and the new ones.
  if (old_listener) {
    nw_listener_cancel(old_listener.get());
    std::unique_lock lock(old_opening->mutex);
    old_opening->changed.wait_for(lock, core->options.advertise_timeout,
                                  [&] { return old_opening->cancelled; });
  }
  auto opening = std::make_shared<Opening>();
  NwRef<nw_listener_t> listener = open_listener(core, keys, port, opening);
  std::string failure = wait_opened(*opening, core->options.advertise_timeout);
  if (!failure.empty()) {
    // The port could not be bound again: any free port, which the owner
    // advertises anew.
    nw_listener_cancel(listener.get());
    opening = std::make_shared<Opening>();
    listener = open_listener(core, keys, 0, opening);
    failure = wait_opened(*opening, core->options.advertise_timeout);
  }
  if (!failure.empty()) {
    nw_listener_cancel(listener.get());
    throw RemoteTransportError(RemoteErrorCode::listener_failed, failure);
  }
  const std::lock_guard opened(opening->mutex);
  const std::lock_guard lock(core->mutex);
  if (core->stopping) {
    nw_listener_cancel(listener.get());
    return;
  }
  core->listener = listener;
  core->opening = opening;
  core->port = opening->port;
}

std::uint16_t RemoteListener::port() const {
  const std::lock_guard lock(core_->mutex);
  return core_->port;
}

std::string RemoteListener::advertised_name() const {
  const std::lock_guard lock(core_->mutex);
  return core_->advertised_name;
}

std::size_t RemoteListener::sessions_started() const {
  const std::lock_guard lock(core_->mutex);
  return static_cast<std::size_t>(core_->sessions_started);
}

std::size_t RemoteListener::active_sessions() const {
  const std::lock_guard lock(core_->mutex);
  return static_cast<std::size_t>(std::count_if(
      core_->sessions.begin(), core_->sessions.end(),
      [](const std::shared_ptr<Core::Session>& session) { return !session->done; }));
}

std::size_t RemoteListener::route_probes_served() const {
  const std::lock_guard lock(core_->mutex);
  return core_->route_probes_served;
}

std::size_t RemoteListener::handshakes_rejected() const {
  const std::lock_guard lock(core_->mutex);
  return core_->handshakes_rejected;
}

void RemoteListener::stop() {
  std::list<std::shared_ptr<Core::Session>> sessions;
  NwRef<nw_listener_t> listener;
  {
    const std::lock_guard lock(core_->mutex);
    if (core_->stopping) {
      return;
    }
    core_->stopping = true;
    sessions.swap(core_->sessions);
    listener = std::move(core_->listener);
  }
  if (listener) {
    nw_listener_cancel(listener.get());
  }
  for (const auto& session : sessions) {
    session->stream->cancel();
  }
  for (const auto& session : sessions) {
    session->thread.join();
  }
}

}  // namespace svp::exec::remote
