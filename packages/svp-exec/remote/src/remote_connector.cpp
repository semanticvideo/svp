#include "svp/exec/remote/remote_connector.hpp"

#include "bonjour_browser.hpp"
#include "interface_link_rate.hpp"
#include "nw_stream.hpp"
#include "route_measurement.hpp"
#include "service_resolver.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "tls_psk_parameters.hpp"
#include "wait_signal.hpp"

#include <chrono>

namespace svp::exec::remote {

using detail::NwRef;
using detail::NwStream;

struct RemoteConnector::State {
  RemoteConnectorOptions options;
  std::shared_ptr<detail::WaitSignal> signal = std::make_shared<detail::WaitSignal>();
};

namespace {

// One resolved address of one discovered service on one interface.
struct Candidate {
  std::string service_name;
  detail::RouteTarget target;
};

// Resolves every (service, interface) the policy allows to addresses; every
// address becomes a candidate pinned to its interface.
std::vector<Candidate> resolve_candidates(const std::vector<detail::DiscoveredService>& services,
                                          const RoutePolicy& policy,
                                          const std::shared_ptr<detail::WaitSignal>& signal) {
  struct Slot {
    const detail::DiscoveredService* service;
    NwRef<nw_interface_t> interface;
    RouteCandidate route;
  };
  std::vector<Slot> slots;
  std::vector<detail::ServiceOnInterface> requests;
  for (const detail::DiscoveredService& service : services) {
    const detail::ServiceOnInterface base{
        .name = service.name, .type = service.type, .domain = service.domain};
    if (service.interfaces.empty()) {
      // Discovery reported no interface; let the system choose one.
      slots.push_back(Slot{.service = &service, .interface = {}, .route = {}});
      requests.push_back(base);
      continue;
    }
    for (const NwRef<nw_interface_t>& interface : service.interfaces) {
      RouteCandidate route{.interface_name = nw_interface_get_name(interface.get()),
                           .medium = detail::route_medium_of(interface.get())};
      if (!route_tier(policy, route.medium)) {
        continue;
      }
      route.link_rate_bps = detail::interface_link_rate_bps(route.interface_name);
      detail::ServiceOnInterface request = base;
      request.interface_index = nw_interface_get_index(interface.get());
      slots.push_back(Slot{.service = &service, .interface = interface, .route = std::move(route)});
      requests.push_back(std::move(request));
    }
  }
  const std::vector<std::vector<detail::ResolvedAddress>> resolved = detail::resolve_services(
      requests, policy.discovery_timeout, policy.discovery_settle, signal);
  std::vector<Candidate> candidates;
  for (std::size_t index = 0; index < slots.size(); ++index) {
    for (const detail::ResolvedAddress& address : resolved[index]) {
      Candidate candidate{.service_name = slots[index].service->name,
                          .target = {.interface = slots[index].interface,
                                     .address = address.address,
                                     .route = slots[index].route}};
      candidate.target.route.address = address.text;
      candidates.push_back(std::move(candidate));
    }
  }
  return candidates;
}

std::vector<detail::DiscoveredService> browse(RemoteConnector::State& state) {
  {
    const std::lock_guard lock(state.signal->mutex);
    if (state.signal->cancelled) {
      throw RemoteTransportError(RemoteErrorCode::cancelled, "connector was cancelled");
    }
  }
  std::vector<detail::DiscoveredService> services =
      state.options.worker_id.empty()
          ? detail::browse_for_pairing(state.options.pairing.pairing_id, state.options.routes,
                                       state.signal)
          : detail::browse_for_worker(state.options.worker_id, state.options.pairing.pairing_id,
                                      state.options.routes, state.signal);
  if (services.empty()) {
    throw RemoteTransportError(RemoteErrorCode::worker_not_found,
                               "no worker advertised pairing `" +
                                   state.options.pairing.pairing_id + "` within " +
                                   std::to_string(state.options.routes.discovery_timeout.count()) +
                                   " ms");
  }
  return services;
}

}  // namespace

std::vector<AdvertisedService> browse_advertised_services(std::string_view key,
                                                          std::string_view value,
                                                          const RoutePolicy& policy) {
  validate_route_policy(policy);
  const auto signal = std::make_shared<detail::WaitSignal>();
  std::vector<AdvertisedService> services;
  for (detail::DiscoveredService& service : detail::browse_for_txt(key, value, policy, signal)) {
    services.push_back(AdvertisedService{.service_name = std::move(service.name),
                                         .txt = std::move(service.txt)});
  }
  return services;
}

RemoteConnector::RemoteConnector(RemoteConnectorOptions options)
    : state_(std::make_shared<State>()) {
  validate_pairing_key(options.pairing);
  validate_route_policy(options.routes);
  validate_transport_policy(options.transport);
  state_->options = std::move(options);
}

RemoteConnector::~RemoteConnector() = default;

void RemoteConnector::cancel() { state_->signal->cancel(); }

std::vector<DiscoveredWorker> RemoteConnector::discover() {
  const std::vector<detail::DiscoveredService> services = browse(*state_);
  std::vector<DiscoveredWorker> workers;
  for (const detail::DiscoveredService& service : services) {
    workers.push_back(DiscoveredWorker{.service_name = service.name, .candidates = {}});
  }
  for (Candidate& candidate :
       resolve_candidates(services, state_->options.routes, state_->signal)) {
    for (DiscoveredWorker& worker : workers) {
      if (worker.service_name == candidate.service_name) {
        worker.candidates.push_back(std::move(candidate.target.route));
      }
    }
  }
  return workers;
}

RemoteConnection RemoteConnector::connect() {
  const RemoteConnectorOptions& options = state_->options;
  const std::shared_ptr<detail::WaitSignal> signal = state_->signal;
  const std::vector<Candidate> candidates =
      resolve_candidates(browse(*state_), options.routes, signal);
  if (candidates.empty()) {
    throw RemoteTransportError(RemoteErrorCode::no_route,
                               "pairing `" + options.pairing.pairing_id +
                                   "` resolved to no address on an interface the route "
                                   "policy allows");
  }
  std::vector<detail::RouteTarget> targets;
  for (const Candidate& candidate : candidates) {
    targets.push_back(candidate.target);
  }
  detail::RouteMeasurement measurement = detail::measure_routes(
      targets, options.pairing, options.transport, options.routes, signal);
  if (!measurement.winner) {
    std::string reasons;
    for (const RouteProbe& probe : measurement.probes) {
      reasons += (reasons.empty() ? "" : "; ") +
                 (probe.candidate.interface_name.empty() ? std::string("any interface")
                                                         : probe.candidate.interface_name) +
                 " " + probe.candidate.address + ": " + probe.failure;
    }
    throw RemoteTransportError(measurement.any_tls_failure
                                   ? RemoteErrorCode::authentication_failed
                                   : RemoteErrorCode::no_route,
                               "pairing `" + options.pairing.pairing_id + "`: " + reasons);
  }

  // The session gets its own connection on the chosen route.
  const Candidate& chosen = candidates[*measurement.winner];
  auto stream = std::make_unique<NwStream>(
      detail::make_pinned_connection(chosen.target, options.pairing, options.transport,
                                     kSessionAlpn),
      [signal] { signal->notify(); });
  stream->start();
  const auto deadline = std::chrono::steady_clock::now() + options.transport.handshake_timeout;
  bool cancelled = false;
  {
    std::unique_lock lock(signal->mutex);
    signal->changed.wait_until(lock, deadline, [&] {
      return signal->cancelled || stream->phase() != detail::StreamPhase::connecting;
    });
    cancelled = signal->cancelled;
  }
  if (cancelled) {
    stream->cancel();
    throw RemoteTransportError(RemoteErrorCode::cancelled, "connect was cancelled");
  }
  const bool ready = stream->phase() == detail::StreamPhase::ready;
  const TlsSession tls = stream->tls_session();
  if (!ready || !detail::is_expected_tls_session(tls) || tls.application_protocol != kSessionAlpn) {
    stream->cancel();
    throw RemoteTransportError(
        stream->failed_in_tls() ? RemoteErrorCode::authentication_failed : RemoteErrorCode::no_route,
        "session connection over " + chosen.target.route.interface_name + " " +
            chosen.target.route.address + " failed: " +
            (ready ? "unexpected TLS session" : stream->failure()));
  }
  RemoteConnection connection;
  connection.route = RouteChoice{.service_name = chosen.service_name,
                                 .route = chosen.target.route,
                                 .handshake = stream->handshake_time(),
                                 .probes = std::move(measurement.probes)};
  connection.stream = std::move(stream);
  return connection;
}

}  // namespace svp::exec::remote
