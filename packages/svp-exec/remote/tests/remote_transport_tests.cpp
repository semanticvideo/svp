// Remote transport on one Mac: TLS 1.2 PSK round trip, wrong secret
// rejected, Bonjour discovery by pairing id, cancellation, and teardown.

#include "exec_test_support.hpp"
#include "pairing_test_support.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/remote_listener.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

namespace {

using namespace svp::exec;
using namespace svp::exec::remote;
using svp::exec::test::expect;
using svp::exec::test::expect_equal;
using svp::exec::test::run_tests;
namespace pairing = svp::exec::remote::test;

// Upper bound for any asynchronous condition; far above a healthy run.
constexpr std::chrono::milliseconds kWaitLimit{10'000};
// Unknown pairings are expected not to be found; a shorter discovery keeps
// that test fast without changing what it proves.
constexpr std::chrono::milliseconds kShortDiscovery{1'000};
// Payload large enough to span many TLS records and receive deliveries.
constexpr std::size_t kEchoPayloadBytes = 3U * 1024U * 1024U + 17U;

void echo_frames(RemoteStream& stream, const RemoteSessionInfo&) {
  StreamFrameReader reader(stream);
  StreamFrameWriter writer(stream);
  while (auto frame = reader.read()) {
    writer.write(*frame);
  }
}

RemoteListenerOptions listener_options(const PairingKey& key) {
  // Named after the pairing so parallel test runs never rename each other.
  return RemoteListenerOptions{.pairing = key, .service_name = key.pairing_id};
}

void wait_for(const std::function<bool()>& condition, std::string_view what) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitLimit;
  while (!condition()) {
    expect(std::chrono::steady_clock::now() < deadline, "timed out waiting for " + std::string(what));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

template <typename Function>
void expect_remote_error(RemoteErrorCode expected, Function&& function, std::string_view what) {
  try {
    function();
  } catch (const RemoteTransportError& error) {
    expect(error.code() == expected, std::string(what) + ": expected " +
                                         std::string(remote_error_code_name(expected)) +
                                         ", got " + error.what());
    return;
  }
  throw std::runtime_error(std::string(what) + ": no RemoteTransportError");
}

void print_route(const RouteChoice& route) {
  std::cout << "  route: service=" << route.service_name << " interface="
            << route.route.interface_name << " address=" << route.route.address
            << " medium=" << route_medium_name(route.route.medium)
            << " session_handshake_us=" << route.handshake.count() << "\n";
  for (const RouteProbe& probe : route.probes) {
    std::cout << "    probe " << probe.candidate.interface_name << " " << probe.candidate.address
              << " up=" << probe.upload_bytes_per_second / 1.0e6
              << " MB/s down=" << probe.download_bytes_per_second / 1.0e6 << " MB/s "
              << probe.failure << "\n";
  }
}

void test_tls_psk_round_trip() {
  const PairingKey key = pairing::random_pairing("svp-transport");
  RemoteListener listener(listener_options(key), echo_frames);
  listener.start();
  expect(listener.port() != 0, "listener has a port");
  expect_equal(listener.advertised_name(), key.pairing_id, "advertised under the given name");

  RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
  RemoteConnection connection = connector.connect();
  print_route(connection.route);
  const TlsSession tls = connection.stream->tls_session();
  expect(tls.protocol_version == kTls12ProtocolVersion &&
             tls.cipher_suite == kTlsPskWithAes128GcmSha256,
         "TLS 1.2 with TLS_PSK_WITH_AES_128_GCM_SHA256");
  expect_equal(tls.application_protocol, kSessionAlpn, "worker session protocol");
  expect_equal(connection.route.service_name, listener.advertised_name(),
               "connected to the advertised service");

  StreamFrameReader reader(*connection.stream);
  StreamFrameWriter writer(*connection.stream);
  const Frame sent{.type = MessageType::blob_put,
                   .body = nlohmann::json{{"probe", "round trip"}},
                   .payloads = {pairing::random_bytes(kEchoPayloadBytes), pairing::random_bytes(1)}};
  writer.write(sent);
  const std::optional<Frame> echoed = reader.read();
  expect(echoed && *echoed == sent, "frame and payloads came back byte-identical");

  connection.stream->cancel();
  wait_for([&] { return listener.active_sessions() == 0; },
           "the worker session to see our cancel as end of stream");
  expect(listener.sessions_started() == 1, "one worker session served");
  expect(listener.route_probes_served() >= 1, "routes were measured by probes, not sessions");
  expect(listener.handshakes_rejected() == 0, "no handshake rejected");
}

void test_wrong_secret_is_rejected() {
  const PairingKey key = pairing::random_pairing("svp-transport");
  RemoteListener listener(listener_options(key), echo_frames);
  listener.start();

  PairingKey impostor = key;
  impostor.secret = pairing::random_bytes(kMinPairingSecretBytes);
  RemoteConnector connector(RemoteConnectorOptions{.pairing = impostor});
  expect_remote_error(RemoteErrorCode::authentication_failed, [&] { (void)connector.connect(); },
                      "wrong secret");
  wait_for([&] { return listener.handshakes_rejected() >= 1; }, "the listener to reject");
  expect(listener.sessions_started() == 0, "no session reached the handler");
}

void test_discovery_by_pairing_id() {
  const PairingKey first = pairing::random_pairing("svp-transport-a");
  const PairingKey second = pairing::random_pairing("svp-transport-b");
  RemoteListener listener_a(listener_options(first), echo_frames);
  RemoteListener listener_b(listener_options(second), echo_frames);
  listener_a.start();
  listener_b.start();

  RemoteConnector connector(RemoteConnectorOptions{.pairing = second});
  const std::vector<DiscoveredWorker> workers = connector.discover();
  expect(workers.size() == 1, "exactly one service advertises the pairing");
  expect_equal(workers.front().service_name, listener_b.advertised_name(),
               "the service that advertises it");
  expect(!workers.front().candidates.empty(), "at least one route");
  for (const RouteCandidate& candidate : workers.front().candidates) {
    std::cout << "  candidate: " << candidate.interface_name << " " << candidate.address << " "
              << route_medium_name(candidate.medium) << " " << candidate.link_rate_bps
              << " bps\n";
  }

  RemoteConnection connection = connector.connect();
  StreamFrameReader reader(*connection.stream);
  StreamFrameWriter writer(*connection.stream);
  writer.write(Frame{.type = MessageType::hello, .body = nlohmann::json::object(), .payloads = {}});
  expect(reader.read().has_value(), "echo from the paired worker");
  expect(listener_b.sessions_started() == 1 && listener_a.sessions_started() == 0 &&
             listener_a.route_probes_served() == 0,
         "only the paired worker was contacted");

  PairingKey unknown = pairing::random_pairing("svp-transport-none");
  RoutePolicy quick;
  quick.discovery_timeout = kShortDiscovery;
  RemoteConnector nobody(RemoteConnectorOptions{.pairing = unknown, .routes = quick});
  expect_remote_error(RemoteErrorCode::worker_not_found, [&] { (void)nobody.discover(); },
                      "unknown pairing");
}

void test_cancelled_connector() {
  const PairingKey key = pairing::random_pairing("svp-transport");
  RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
  connector.cancel();
  expect_remote_error(RemoteErrorCode::cancelled, [&] { (void)connector.connect(); },
                      "cancelled before connect");
}

void test_cancel_interrupts_discovery() {
  const PairingKey key = pairing::random_pairing("svp-transport-none");
  RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
  const auto started = std::chrono::steady_clock::now();
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    connector.cancel();
  });
  expect_remote_error(RemoteErrorCode::cancelled, [&] { (void)connector.connect(); },
                      "cancelled during discovery");
  canceller.join();
  expect(std::chrono::steady_clock::now() - started < kDefaultDiscoveryTimeout,
         "cancel did not wait for the discovery timeout");
}

void test_listener_stop_ends_sessions() {
  const PairingKey key = pairing::random_pairing("svp-transport");
  RemoteListener listener(listener_options(key), echo_frames);
  listener.start();
  RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
  RemoteConnection connection = connector.connect();
  StreamFrameReader reader(*connection.stream);
  wait_for([&] { return listener.active_sessions() >= 1; }, "the session to start");
  listener.stop();
  expect(!reader.read(), "our side reads end of stream");
}

void test_invalid_pairing_is_refused() {
  PairingKey weak = pairing::random_pairing("svp-transport");
  weak.secret.resize(kMinPairingSecretBytes - 1);
  expect_remote_error(RemoteErrorCode::invalid_configuration,
                      [&] { RemoteConnector connector(RemoteConnectorOptions{.pairing = weak}); },
                      "short secret");
  PairingKey bad_id = pairing::random_pairing("svp-transport");
  bad_id.pairing_id = "has space";
  expect_remote_error(RemoteErrorCode::invalid_configuration,
                      [&] {
                        RemoteListener listener(listener_options(bad_id), echo_frames);
                        listener.start();
                      },
                      "bad pairing id");
}

}  // namespace

int main() {
  return run_tests("svp-exec-remote-transport-tests",
                   {
                       {"TLS-PSK round trip", test_tls_psk_round_trip},
                       {"wrong secret is rejected", test_wrong_secret_is_rejected},
                       {"discovery by pairing id", test_discovery_by_pairing_id},
                       {"cancelled connector", test_cancelled_connector},
                       {"cancel interrupts discovery", test_cancel_interrupts_discovery},
                       {"listener stop ends sessions", test_listener_stop_ends_sessions},
                       {"invalid pairing is refused", test_invalid_pairing_is_refused},

                   });
}
