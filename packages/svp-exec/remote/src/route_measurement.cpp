#include "route_measurement.hpp"

#include "nw_stream.hpp"
#include "route_probe_wire.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "tls_psk_parameters.hpp"

#include <algorithm>
#include <chrono>
#include <list>
#include <thread>

namespace svp::exec::remote::detail {
namespace {

using Clock = std::chrono::steady_clock;

struct ProbeSlot {
  std::unique_ptr<NwStream> stream;
  std::thread thread;
  // Guarded by signal->mutex.
  RouteProbe probe;
  bool authenticated = false;
  // When the handshake finished and measuring began.
  Clock::time_point measuring_since{};
  bool finished = false;
  bool tls_failure = false;
};

double bytes_per_second(std::uint64_t bytes, Clock::duration elapsed) {
  // A transfer faster than the clock can resolve counts as one tick.
  const double seconds =
      std::max(std::chrono::duration<double>(elapsed).count(),
               std::chrono::duration<double>(Clock::duration(1)).count());
  return static_cast<double>(bytes) / seconds;
}

// Sends policy.probe_bytes and times their return. Throws ExecError when the
// stream ends or is cancelled first.
void exchange(NwStream& stream, std::uint64_t length, ProbeSlot& slot,
              const std::shared_ptr<WaitSignal>& signal) {
  std::vector<std::byte> chunk(kRouteProbeChunkBytes);
  const auto started = Clock::now();
  stream.write_all(encode_probe_length(length));
  for (std::uint64_t sent = 0; sent < length; sent += chunk.size()) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), length - sent));
    stream.write_all(std::span<const std::byte>(chunk.data(), count));
  }
  std::uint64_t received = 0;
  Clock::time_point first_byte{};
  while (received < length) {
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), length - received));
    const std::size_t count = stream.read_some(std::span(chunk.data(), want));
    if (count == 0) {
      throw ExecError(ExecErrorCode::frame_truncated, "route probe ended early");
    }
    if (received == 0) {
      first_byte = Clock::now();
    }
    received += count;
  }
  const auto ended = Clock::now();
  {
    const std::lock_guard lock(signal->mutex);
    // The worker answers only after all bytes arrived, so the first byte of
    // the answer bounds the upload.
    slot.probe.upload_bytes_per_second = bytes_per_second(length, first_byte - started);
    slot.probe.download_bytes_per_second = bytes_per_second(length, ended - first_byte);
    slot.probe.state = RouteProbeState::ready;
    slot.finished = true;
  }
  signal->changed.notify_all();
}

void run_probe(ProbeSlot& slot, Clock::time_point handshake_deadline, std::uint64_t length,
               const std::shared_ptr<WaitSignal>& signal) {
  NwStream& stream = *slot.stream;
  const bool ready = stream.wait_ready(handshake_deadline);
  const TlsSession tls = stream.tls_session();
  const bool authenticated = ready && is_expected_tls_session(tls) &&
                             tls.application_protocol == kRouteProbeAlpn;
  {
    const std::lock_guard lock(signal->mutex);
    if (authenticated) {
      slot.authenticated = true;
      slot.measuring_since = Clock::now();
      slot.probe.handshake = stream.handshake_time();
    } else {
      slot.finished = true;
      slot.probe.state = RouteProbeState::failed;
      slot.tls_failure = stream.failed_in_tls();
      slot.probe.failure = ready                      ? "peer negotiated an unexpected TLS session"
                           : stream.failure().empty() ? "handshake timed out"
                                                      : stream.failure();
    }
  }
  signal->changed.notify_all();
  if (!authenticated) {
    stream.cancel();
    return;
  }
  try {
    exchange(stream, length, slot, signal);
  } catch (const ExecError& error) {
    {
      const std::lock_guard lock(signal->mutex);
      slot.finished = true;
      slot.probe.failure = std::string("throughput probe: ") + error.what();
    }
    signal->changed.notify_all();
  }
  stream.cancel();
}

}  // namespace

NwRef<nw_connection_t> make_pinned_connection(const RouteTarget& target, const PairingKey& key,
                                              const TransportPolicy& transport,
                                              std::string_view application_protocol) {
  const NwRef<nw_parameters_t> parameters =
      make_tls_psk_parameters(key, transport, {std::string(application_protocol)});
  if (target.interface) {
    nw_parameters_require_interface(parameters.get(), target.interface.get());
  }
  const auto endpoint = NwRef<nw_endpoint_t>::adopt(
      nw_endpoint_create_address(reinterpret_cast<const sockaddr*>(&target.address)));
  return NwRef<nw_connection_t>::adopt(nw_connection_create(endpoint.get(), parameters.get()));
}

RouteMeasurement measure_routes(const std::vector<RouteTarget>& targets, const PairingKey& key,
                                const TransportPolicy& transport, const RoutePolicy& policy,
                                const std::shared_ptr<WaitSignal>& signal) {
  // std::list: probe threads hold references to their slot.
  std::list<ProbeSlot> slots;
  for (const RouteTarget& target : targets) {
    ProbeSlot& slot = slots.emplace_back();
    slot.probe.candidate = target.route;
    slot.stream = std::make_unique<NwStream>(
        make_pinned_connection(target, key, transport, kRouteProbeAlpn),
        [signal] { signal->notify(); });
  }
  const auto started = Clock::now();
  const auto handshake_deadline = started + transport.handshake_timeout;
  for (ProbeSlot& slot : slots) {
    slot.stream->start();
    slot.thread = std::thread(run_probe, std::ref(slot), handshake_deadline, policy.probe_bytes,
                              signal);
  }

  RouteMeasurement measurement;
  bool cancelled = false;
  {
    std::unique_lock lock(signal->mutex);
    while (true) {
      const auto now = Clock::now();
      cancelled = signal->cancelled;
      // A probe is settled when its thread finished, or when its handshake or
      // measuring time ran out.
      const auto settled = [&](const ProbeSlot& slot) {
        return slot.finished ||
               (slot.authenticated ? now >= slot.measuring_since + policy.probe_timeout
                                   : now >= handshake_deadline);
      };
      const bool final = std::all_of(slots.begin(), slots.end(), settled);
      measurement.probes.clear();
      measurement.any_tls_failure = false;
      for (const ProbeSlot& slot : slots) {
        RouteProbe probe = slot.probe;
        if (probe.state == RouteProbeState::pending && slot.authenticated) {
          probe.measuring = std::chrono::duration_cast<std::chrono::microseconds>(
              now - slot.measuring_since);
        }
        if (probe.state == RouteProbeState::pending && settled(slot)) {
          // Authenticated but unmeasured routes stay usable, ranked last in
          // their tier; the rest never connected in time.
          probe.state = slot.authenticated ? RouteProbeState::ready : RouteProbeState::failed;
          probe.failure = slot.authenticated ? "throughput probe did not finish in time"
                                             : "handshake timed out";
        }
        measurement.any_tls_failure = measurement.any_tls_failure || slot.tls_failure;
        measurement.probes.push_back(std::move(probe));
      }
      if (cancelled) {
        break;
      }
      measurement.winner = select_route(policy, measurement.probes, final);
      if (measurement.winner || final) {
        break;
      }
      // Wake for the next deadline, or when a measuring probe's throughput
      // bound falls below the best measured so far.
      auto wake = handshake_deadline;
      double best = 0.0;
      for (const RouteProbe& probe : measurement.probes) {
        if (probe.state == RouteProbeState::ready) {
          best = std::max(best, probe.throughput());
        }
      }
      for (const ProbeSlot& slot : slots) {
        if (!slot.finished && slot.authenticated) {
          wake = std::min(wake, slot.measuring_since + policy.probe_timeout);
          if (best > 0.0) {
            const auto cannot_win = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(2.0 * static_cast<double>(policy.probe_bytes) / best));
            wake = std::min(wake, slot.measuring_since + cannot_win);
          }
        }
      }
      signal->changed.wait_until(lock, std::max(wake, now + std::chrono::milliseconds(1)));
    }
  }
  for (ProbeSlot& slot : slots) {
    slot.stream->cancel();
  }
  for (ProbeSlot& slot : slots) {
    slot.thread.join();
  }
  if (cancelled) {
    throw RemoteTransportError(RemoteErrorCode::cancelled, "route measurement was cancelled");
  }
  return measurement;
}

}  // namespace svp::exec::remote::detail
