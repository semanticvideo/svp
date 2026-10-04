// Remote transport on one Mac: TLS 1.2 PSK round trip, wrong secret
// rejected, Bonjour discovery by pairing id, cancellation, and teardown.

#include "exec_test_support.hpp"
#include "nw_stream.hpp"
#include "pairing_test_support.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/remote/service_advertiser.hpp"

#include <sys/sysctl.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

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

// A single write larger than everything between a writer and a reader that
// stopped reading can hold: the reader's read-ahead (kReceiveAheadBytes) plus
// the kernel's send and receive buffers, each at most kern.ipc.maxsockbuf.
// Such a write cannot complete until the reader reads, so it is provably
// still in flight when the reader goes away.
std::size_t unfinishable_write_bytes() {
  std::uint64_t max_socket_buffer = 0;
  std::size_t size = sizeof(max_socket_buffer);
  if (sysctlbyname("kern.ipc.maxsockbuf", &max_socket_buffer, &size, nullptr, 0) != 0 ||
      max_socket_buffer == 0) {
    throw std::runtime_error("cannot read kern.ipc.maxsockbuf");
  }
  return svp::exec::remote::detail::kReceiveAheadBytes +
         2U * static_cast<std::size_t>(max_socket_buffer) + 1U;
}

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
  // Counts route probe connections too: those the coordinator cancelled
  // mid-reply must end as well (see the next test).
  wait_for([&] { return listener.active_sessions() == 0; },
           "the worker session and every route probe to end after our cancel");
  expect(listener.sessions_started() == 1, "one worker session served");
  expect(listener.route_probes_served() >= 1, "routes were measured by probes, not sessions");
  expect(listener.handshakes_rejected() == 0, "no handshake rejected");
}

// A peer that resets the connection while our write is in flight must fail
// that write. A send waiting for the peer's window when the connection fails
// (here: ECONNRESET) is never completed by Network framework until the
// connection is cancelled, so without that the worker session blocks in
// write_all until the listener stops. Route probes hit this when the
// coordinator picks a winner and cancels the probes still replying.
void test_peer_reset_fails_a_write_in_flight() {
  const PairingKey key = pairing::random_pairing("svp-transport");
  const std::size_t write_bytes = unfinishable_write_bytes();
  std::atomic<bool> write_completed{false};
  std::atomic<bool> write_failed{false};
  RemoteListener listener(listener_options(key), [&](RemoteStream& stream, const RemoteSessionInfo&) {
    const std::vector<std::byte> payload(write_bytes);
    try {
      stream.write_all(payload);
      write_completed = true;
    } catch (const ExecError&) {
      write_failed = true;
    }
  });
  listener.start();

  RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
  RemoteConnection connection = connector.connect();
  // One byte proves the worker's write is on the wire; it cannot finish
  // because we read nothing more.
  std::byte first{};
  expect(connection.stream->read_some(std::span(&first, 1)) == 1, "the worker started writing");
  connection.stream->cancel();
  wait_for([&] { return listener.active_sessions() == 0; },
           "the worker's write in flight to fail when we reset the connection");
  expect(write_failed && !write_completed, "the write failed instead of completing");
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

void test_browse_by_txt_entry() {
  const PairingKey first = pairing::random_pairing("svp-transport-txt-a");
  const PairingKey second = pairing::random_pairing("svp-transport-txt-b");
  const PairingKey outsider = pairing::random_pairing("svp-transport-txt-c");
  // A value unique to this run, so parallel runs never see each other.
  const std::string group = first.pairing_id;
  RemoteListenerOptions a = listener_options(first);
  a.txt = {{"group", group}, {"extra", "1"}};
  RemoteListenerOptions b = listener_options(second);
  b.txt = {{"group", group}};
  RemoteListenerOptions c = listener_options(outsider);
  c.txt = {{"group", group + "-other"}};
  RemoteListener listener_a(a, echo_frames);
  RemoteListener listener_b(b, echo_frames);
  RemoteListener listener_c(c, echo_frames);
  listener_a.start();
  listener_b.start();
  listener_c.start();
  const std::vector<AdvertisedService> services = browse_advertised_services("group", group);
  std::set<std::string> pairing_ids;
  for (const AdvertisedService& service : services) {
    pairing_ids.insert(service.txt.at(std::string(kPairingTxtKey)));
    if (service.txt.at(std::string(kPairingTxtKey)) == first.pairing_id) {
      expect(service.txt.count("extra") == 1 && service.txt.at("extra") == "1",
             "every TXT entry is reported");
    }
  }
  expect(pairing_ids == std::set<std::string>{first.pairing_id, second.pairing_id},
         "both services with the entry, and only those, are found");

  RemoteListenerOptions bad = listener_options(outsider);
  bad.txt = {{std::string(kPairingTxtKey), "spoof"}};
  expect_remote_error(RemoteErrorCode::invalid_configuration,
                      [&] {
                        RemoteListener listener(bad, echo_frames);
                        listener.start();
                      },
                      "the pairing entry cannot be overridden");
  RemoteListenerOptions long_entry = listener_options(outsider);
  long_entry.txt = {{"k", std::string(kMaxTxtEntryBytes, 'x')}};
  expect_remote_error(RemoteErrorCode::invalid_configuration,
                      [&] {
                        RemoteListener listener(long_entry, echo_frames);
                        listener.start();
                      },
                      "an entry over 255 bytes");
}

void test_one_listener_accepts_many_keys() {
  // The advertised key is the last of nine the listener holds, so a TLS
  // stack that did not pick the PSK by the client's identity would fail.
  const PairingKey advertised = pairing::random_pairing("svp-transport-many");
  RemoteListenerOptions options = listener_options(advertised);
  for (int index = 0; index < 8; ++index) {
    options.accepted_keys.push_back(
        pairing::random_pairing("svp-transport-many-" + std::to_string(index)));
  }
  const PairingKey other = options.accepted_keys[3];
  RemoteListener listener(options, echo_frames);
  listener.start();

  RemoteConnector connector(RemoteConnectorOptions{.pairing = advertised});
  RemoteConnection connection = connector.connect();
  StreamFrameReader reader(*connection.stream);
  StreamFrameWriter writer(*connection.stream);
  writer.write(Frame{.type = MessageType::hello, .body = nlohmann::json::object(), .payloads = {}});
  expect(reader.read().has_value(), "a session over a key that is not the first");

  PairingKey wrong = advertised;
  wrong.secret = other.secret;
  RemoteConnector impostor(RemoteConnectorOptions{.pairing = wrong});
  expect_remote_error(RemoteErrorCode::authentication_failed, [&] { (void)impostor.connect(); },
                      "another accepted key's secret under this identity is refused");
  expect(listener.sessions_started() == 1, "only the right key opened a session");
}

std::string hex_of(const std::vector<std::byte>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  for (const std::byte byte : bytes) {
    hex += kDigits[static_cast<unsigned>(byte) >> 4U];
    hex += kDigits[static_cast<unsigned>(byte) & 0xFU];
  }
  return hex;
}

// Echoes frames, then reports this connection's TLS exporter as a final
// frame so the client can compare it with its own.
void echo_with_exporter(RemoteStream& stream, const RemoteSessionInfo&) {
  StreamFrameReader reader(stream);
  StreamFrameWriter writer(stream);
  if (auto frame = reader.read()) {
    const auto exporter = stream.export_keying_material("EXPORTER-svp-test", 32);
    const std::string hex = exporter ? hex_of(*exporter) : std::string();
    writer.write(Frame{.type = MessageType::hello, .body = {{"exporter", hex}}, .payloads = {}});
  }
}

void test_exporter_is_shared_and_per_connection() {
  const PairingKey key = pairing::random_pairing("svp-transport-exporter");
  RemoteListener listener(listener_options(key), echo_with_exporter);
  listener.start();
  std::set<std::string> seen;
  for (int round = 0; round < 2; ++round) {
    RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
    RemoteConnection connection = connector.connect();
    const auto mine = connection.stream->export_keying_material("EXPORTER-svp-test", 32);
    expect(mine.has_value() && mine->size() == 32, "the client has an exporter");
    StreamFrameReader reader(*connection.stream);
    StreamFrameWriter writer(*connection.stream);
    writer.write(Frame{.type = MessageType::hello, .body = nlohmann::json::object(), .payloads = {}});
    const auto answer = reader.read();
    expect(answer.has_value(), "the server answered");
    expect_equal(answer->body["exporter"].get<std::string>(), hex_of(*mine),
                 "both ends derive the same exporter");
    seen.insert(hex_of(*mine));
  }
  expect(seen.size() == 2, "every connection has its own exporter");
}

void test_unadvertised_listener_replaces_its_keys() {
  const PairingKey first = pairing::random_pairing("svp-transport-replace-a");
  const PairingKey second = pairing::random_pairing("svp-transport-replace-b");
  RemoteListenerOptions options;
  options.pairing = first;
  options.advertise = false;
  RemoteListener listener(options, echo_frames);
  listener.start();
  const std::uint16_t port = listener.port();
  expect(port != 0, "listening");
  // Advertised under both pairing ids, as a worker does for older
  // coordinators, on the one port.
  ServiceAdvertiser ad_first(ServiceAdvertisement{
      .name = first.pairing_id, .port = port, .txt = {{"pairing", first.pairing_id}}});
  ServiceAdvertiser ad_second(ServiceAdvertisement{
      .name = second.pairing_id, .port = port, .txt = {{"pairing", second.pairing_id}}});
  expect(ad_first.wait_registered(std::chrono::seconds(10)) &&
             ad_second.wait_registered(std::chrono::seconds(10)),
         "both instances registered");

  const auto session_with = [](const PairingKey& key) {
    RemoteConnector connector(RemoteConnectorOptions{.pairing = key});
    RemoteConnection connection = connector.connect();
    StreamFrameReader reader(*connection.stream);
    StreamFrameWriter writer(*connection.stream);
    writer.write(Frame{.type = MessageType::hello, .body = nlohmann::json::object(), .payloads = {}});
    return reader.read().has_value();
  };
  expect(session_with(first), "the first key works");
  expect_remote_error(RemoteErrorCode::authentication_failed, [&] { (void)session_with(second); },
                      "the second key is not accepted yet");

  // A session open across the replacement keeps working.
  RemoteConnector open_connector(RemoteConnectorOptions{.pairing = first});
  RemoteConnection open = open_connector.connect();
  StreamFrameReader open_reader(*open.stream);
  StreamFrameWriter open_writer(*open.stream);

  listener.replace_keys(second, {first});
  expect(listener.port() == port, "the replacement listens on the same port");
  expect(session_with(second), "an added key works at once");
  expect(session_with(first), "a kept key still works");
  open_writer.write(Frame{.type = MessageType::hello, .body = nlohmann::json::object(), .payloads = {}});
  expect(open_reader.read().has_value(), "a session opened before the replacement goes on");

  listener.replace_keys(second, {});
  expect_remote_error(RemoteErrorCode::authentication_failed, [&] { (void)session_with(first); },
                      "a removed key is refused");
  expect(session_with(second), "the remaining key works");

  RemoteListener advertising(listener_options(first), echo_frames);
  expect_remote_error(RemoteErrorCode::invalid_configuration,
                      [&] { advertising.replace_keys(first, {}); },
                      "an advertising listener cannot change its keys");
}

void test_a_spoofed_advertisement_does_not_displace_the_worker() {
  const PairingKey genuine_key = pairing::random_pairing("svp-transport-genuine");
  const PairingKey spoof_key = pairing::random_pairing("svp-transport-spoof");
  const std::string worker_id = "svpn-" + genuine_key.pairing_id;
  RemoteListenerOptions genuine_options;
  genuine_options.pairing = genuine_key;
  genuine_options.advertise = false;
  RemoteListener genuine(genuine_options, echo_frames);
  genuine.start();
  RemoteListenerOptions spoof_options;
  spoof_options.pairing = spoof_key;
  spoof_options.advertise = false;
  RemoteListener spoof(spoof_options, echo_frames);
  spoof.start();
  // The spoof advertises the worker's id under a name that sorts first and
  // without a pairing entry, as a worker's own instance looks.
  ServiceAdvertiser spoof_ad(ServiceAdvertisement{
      .name = "0-" + genuine_key.pairing_id, .port = spoof.port(), .txt = {{"worker", worker_id}}});
  ServiceAdvertiser genuine_ad(ServiceAdvertisement{
      .name = "1-" + genuine_key.pairing_id, .port = genuine.port(), .txt = {{"worker", worker_id}}});
  expect(spoof_ad.wait_registered(std::chrono::seconds(10)) &&
             genuine_ad.wait_registered(std::chrono::seconds(10)),
         "both registered");
  RemoteConnector connector(RemoteConnectorOptions{.pairing = genuine_key, .worker_id = worker_id});
  RemoteConnection connection = connector.connect();
  StreamFrameReader reader(*connection.stream);
  StreamFrameWriter writer(*connection.stream);
  writer.write(Frame{.type = MessageType::hello, .body = nlohmann::json::object(), .payloads = {}});
  expect(reader.read().has_value(), "the coordinator reaches the genuine worker");
  expect(genuine.sessions_started() == 1 && spoof.sessions_started() == 0,
         "the spoofed instance is never served a session");
}

void test_advertiser_registers_and_withdraws() {
  const std::string id = pairing::random_pairing("svp-transport-ad").pairing_id;
  {
    ServiceAdvertiser advertiser(
        ServiceAdvertisement{.name = id, .port = 9, .txt = {{"group", id}, {"extra", "x"}}});
    expect(advertiser.wait_registered(std::chrono::seconds(10)), "registered");
    expect_equal(advertiser.registered_name(), id, "under the chosen name");
    const std::vector<AdvertisedService> found = browse_advertised_services("group", id);
    expect(found.size() == 1 && found.front().txt.at("extra") == "x",
           "browsing finds it with its TXT entries");
  }
  RoutePolicy quick;
  quick.discovery_timeout = kShortDiscovery;
  expect(browse_advertised_services("group", id, quick).empty(),
         "destroying the advertiser withdraws it");
  expect_remote_error(RemoteErrorCode::invalid_configuration,
                      [] { ServiceAdvertiser bad(ServiceAdvertisement{.name = "x", .port = 0}); },
                      "a port is required");
}

void test_advertise_retry_backoff() {
  expect(advertise_retry_delay(1) == kAdvertiseRetryInitial, "the first retry");
  expect(advertise_retry_delay(2) == 2 * kAdvertiseRetryInitial, "doubles");
  expect(advertise_retry_delay(3) == 4 * kAdvertiseRetryInitial, "and doubles");
  std::chrono::milliseconds previous{0};
  for (std::uint32_t attempt = 1; attempt < 64; ++attempt) {
    const std::chrono::milliseconds delay = advertise_retry_delay(attempt);
    expect(delay >= previous && delay <= kAdvertiseRetryMax, "never shrinks, never past the cap");
    previous = delay;
  }
  expect(advertise_retry_delay(1000) == kAdvertiseRetryMax, "capped");
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
                       {"peer reset fails a write in flight",
                        test_peer_reset_fails_a_write_in_flight},
                       {"wrong secret is rejected", test_wrong_secret_is_rejected},
                       {"discovery by pairing id", test_discovery_by_pairing_id},
                       {"browse by TXT entry", test_browse_by_txt_entry},
                       {"one listener accepts many keys", test_one_listener_accepts_many_keys},
                       {"exporter is shared and per connection",
                        test_exporter_is_shared_and_per_connection},
                       {"unadvertised listener replaces its keys",
                        test_unadvertised_listener_replaces_its_keys},
                       {"advertiser registers and withdraws", test_advertiser_registers_and_withdraws},
                       {"a spoofed advertisement does not displace the worker",
                        test_a_spoofed_advertisement_does_not_displace_the_worker},
                       {"advertise retry backoff", test_advertise_retry_backoff},
                       {"cancelled connector", test_cancelled_connector},
                       {"cancel interrupts discovery", test_cancel_interrupts_discovery},
                       {"listener stop ends sessions", test_listener_stop_ends_sessions},
                       {"invalid pairing is refused", test_invalid_pairing_is_refused},

                   });
}
