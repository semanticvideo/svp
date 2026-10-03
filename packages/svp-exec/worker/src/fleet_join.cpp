#include "svp/exec/worker/fleet_join.hpp"

#include "hex_bytes.hpp"
#include "message_fields.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/worker/fleet_crypto.hpp"
#include "svp/exec/worker/hello_messages.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <charconv>
#include <cstring>
#include <nlohmann/json.hpp>

namespace svp::exec::worker {
namespace {

using namespace detail;

std::span<const std::byte> as_bytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

std::vector<std::byte> digest_bytes(const Blake3Digest& digest) {
  const auto* begin = reinterpret_cast<const std::byte*>(digest.data());
  return std::vector<std::byte>(begin, begin + digest.size());
}

Frame make_frame(MessageType type, nlohmann::json body) {
  return Frame{.type = type, .body = std::move(body), .payloads = {}};
}

Frame error_frame(WorkerErrorCode code, const std::string& message) {
  return make_frame(MessageType::error,
                    nlohmann::json{{"code", std::string(worker_error_code_name(code))},
                                   {"message", message}});
}

// The peer ended the join with ERROR: reported to the caller, never echoed.
class PeerRefusal : public WorkerError {
 public:
  explicit PeerRefusal(const std::string& message)
      : WorkerError(WorkerErrorCode::refused, message) {}
};

// Reads the next frame, which must be `expected`; an ERROR from the peer
// becomes PeerRefusal with its message.
nlohmann::json expect_frame(FrameReader& input, MessageType expected) {
  const std::optional<Frame> frame = input.read();
  if (!frame) {
    throw WorkerError(WorkerErrorCode::protocol, "the peer closed the join connection before " +
                                                     std::string(message_type_name(expected)));
  }
  if (frame->type == MessageType::error) {
    const std::string message = frame->body.is_object() && frame->body.contains("message") &&
                                        frame->body["message"].is_string()
                                    ? frame->body["message"].get<std::string>()
                                    : std::string("no reason given");
    throw PeerRefusal("the peer refused the join: " + message);
  }
  (void)require_frame(*frame, expected, 0);
  return frame->body;
}

std::vector<std::byte> hex_bytes_field(const nlohmann::json& body, std::string_view name,
                                       std::size_t bytes, std::string_view path) {
  const std::optional<std::vector<std::byte>> value =
      parse_hex_bytes(required_string(body, name, path), bytes);
  if (!value) {
    throw WorkerError(WorkerErrorCode::protocol, std::string(path) + "." + std::string(name) +
                                                     " must be " + std::to_string(bytes * 2) +
                                                     " lowercase hex digits");
  }
  return *value;
}

struct Transcript {
  std::string coordinator_ephemeral;
  std::string coordinator_id;
  std::string fleet_id;
  std::string pairing_id;
  std::string worker_ephemeral;
  std::string worker_join_id;

  [[nodiscard]] Blake3Digest hash() const {
    return blake3_digest(encode_canonical_json(
        nlohmann::json{{"coordinator_ephemeral", coordinator_ephemeral},
                       {"coordinator_id", coordinator_id},
                       {"fleet_id", fleet_id},
                       {"pairing_id", pairing_id},
                       {"worker_ephemeral", worker_ephemeral},
                       {"worker_join_id", worker_join_id}}));
  }
};

std::vector<std::byte> pairing_secret(const std::vector<std::byte>& shared,
                                      const Blake3Digest& transcript_hash) {
  std::vector<std::byte> material = shared;
  const auto* begin = reinterpret_cast<const std::byte*>(transcript_hash.data());
  material.insert(material.end(), begin, begin + transcript_hash.size());
  return digest_bytes(blake3_derive_key(kJoinPairingSecretContext, material));
}

std::vector<std::byte> wrap_member_key(const std::vector<std::byte>& member,
                                       const std::vector<std::byte>& secret) {
  const Blake3Digest pad = blake3_derive_key(kJoinMemberWrapContext, secret);
  std::vector<std::byte> wrapped(member.size());
  for (std::size_t index = 0; index < member.size(); ++index) {
    wrapped[index] = member[index] ^ static_cast<std::byte>(pad[index]);
  }
  return wrapped;
}

std::string signed_message(const Blake3Digest& transcript_hash, std::string_view wrapped_hex) {
  return std::string(kJoinSignaturePrefix) + blake3_hex(transcript_hash) + "\n" +
         std::string(wrapped_hex);
}

std::string confirmation(const std::vector<std::byte>& secret,
                         const Blake3Digest& transcript_hash) {
  Blake3Digest key{};
  std::memcpy(key.data(), secret.data(), key.size());
  return blake3_hex(blake3_keyed_hash(
      key, as_bytes(std::string(kJoinConfirmPrefix) + blake3_hex(transcript_hash))));
}

nlohmann::json endpoint_to_json(const WorkerEndpoint& endpoint) {
  nlohmann::json body{{"home", endpoint.home},
                        {"label", endpoint.label},
                        {"plist", endpoint.plist},
                        {"root", endpoint.root},
                        {"service_mode", std::string(worker_service_mode_name(endpoint.service_mode))},
                        {"uid", endpoint.uid},
                        {"user", endpoint.user}};
  if (!endpoint.worker_id.empty()) {
    body["worker_id"] = endpoint.worker_id;
  }
  return body;
}

WorkerEndpoint endpoint_from_json(const nlohmann::json& body, std::string_view path) {
  require_object(body, path);
  WorkerEndpoint endpoint;
  endpoint.home = required_string(body, "home", path);
  endpoint.label = required_string(body, "label", path);
  endpoint.plist = required_string(body, "plist", path);
  endpoint.root = required_string(body, "root", path);
  endpoint.uid = required_u32(body, "uid", path);
  endpoint.user = required_string(body, "user", path);
  const std::string mode = required_string(body, "service_mode", path);
  const std::optional<WorkerServiceMode> parsed = parse_worker_service_mode(mode);
  if (!parsed) {
    throw WorkerError(WorkerErrorCode::protocol, std::string(path) + ".service_mode is unknown");
  }
  endpoint.service_mode = *parsed;
  if (body.contains("worker_id")) {
    endpoint.worker_id = required_string(body, "worker_id", path);
  }
  return endpoint;
}

// Runs `body`; a WorkerError or ExecError is reported to the peer with ERROR
// (best effort) and rethrown as WorkerError.
template <typename Function>
auto reporting_errors(FrameWriter& output, Function&& body) {
  try {
    return body();
  } catch (const PeerRefusal&) {
    throw;
  } catch (const WorkerError& error) {
    try {
      output.write(error_frame(error.code(), error.what()));
    } catch (const std::exception&) {
    }
    throw;
  } catch (const ExecError& error) {
    try {
      output.write(error_frame(WorkerErrorCode::protocol, error.what()));
    } catch (const std::exception&) {
    }
    throw WorkerError(WorkerErrorCode::protocol, error.what());
  }
}

}  // namespace

std::string join_txt_value(const WorkerJoinCredential& credential) {
  if (credential.member_key) {
    return std::string(kJoinTxtMember);
  }
  return std::string(kJoinTxtTokenPrefix) + credential.token.token_id + "." +
         std::to_string(credential.token.expires_at);
}

JoinListenerKey coordinator_join_key(const FleetSecret& fleet, std::string_view worker_join_id,
                                     std::string_view join_txt, std::uint64_t now) {
  if (!is_fleet_identifier(worker_join_id, kWorkerJoinIdPrefix)) {
    return {std::nullopt, "malformed worker join id `" + std::string(worker_join_id) + "`"};
  }
  if (join_txt == kJoinTxtMember) {
    return {svp::exec::remote::PairingKey{.pairing_id = std::string(worker_join_id),
                                          .secret = member_key(fleet, worker_join_id)},
            {}};
  }
  if (!join_txt.starts_with(kJoinTxtTokenPrefix)) {
    return {std::nullopt, "unknown join advertisement `" + std::string(join_txt) + "`"};
  }
  const std::string_view rest = join_txt.substr(kJoinTxtTokenPrefix.size());
  const std::size_t dot = rest.find('.');
  std::uint64_t expires_at = 0;
  const std::string_view expiry = dot == std::string_view::npos ? std::string_view{}
                                                                : rest.substr(dot + 1);
  const auto [end, error] =
      std::from_chars(expiry.data(), expiry.data() + expiry.size(), expires_at);
  const std::string_view token_id = rest.substr(0, dot);
  if (dot == std::string_view::npos || !is_fleet_identifier(token_id, "") ||
      error != std::errc{} || end != expiry.data() + expiry.size() || expiry.empty()) {
    return {std::nullopt, "malformed join advertisement `" + std::string(join_txt) + "`"};
  }
  if (now > expires_at) {
    return {std::nullopt, "its join token " + std::string(token_id) +
                              " expired before its first pairing; reinstall it with a new "
                              "token from `svp-builder workers fleet token`"};
  }
  return {svp::exec::remote::PairingKey{.pairing_id = std::string(worker_join_id),
                                        .secret = token_join_key(fleet, token_id, expires_at)},
          {}};
}

WorkerJoinResult serve_fleet_join(FrameReader& input, FrameWriter& output,
                                  const WorkerJoinCredential& credential,
                                  const WorkerEndpoint& self, const HostFacts& host,
                                  const std::function<void(const WorkerJoinResult&)>& persist) {
  return reporting_errors(output, [&] {
    constexpr std::string_view kOffer = "JOIN_OFFER.body";
    const nlohmann::json offer = expect_frame(input, MessageType::join_offer);
    Transcript transcript;
    transcript.fleet_id = required_string(offer, "fleet_id", kOffer);
    transcript.coordinator_id = required_string(offer, "coordinator_id", kOffer);
    const std::vector<std::byte> coordinator_ephemeral =
        hex_bytes_field(offer, "coordinator_ephemeral", kEcPublicKeyBytes, kOffer);
    transcript.coordinator_ephemeral = bytes_hex(coordinator_ephemeral);
    if (transcript.fleet_id != credential.token.fleet_id) {
      throw WorkerError(WorkerErrorCode::refused, "this worker belongs to fleet " +
                                                      credential.token.fleet_id + ", not " +
                                                      transcript.fleet_id);
    }
    if (!is_fleet_identifier(transcript.coordinator_id, kCoordinatorIdPrefix)) {
      throw WorkerError(WorkerErrorCode::protocol, "malformed coordinator id");
    }

    const EcKeyPair ephemeral = generate_ec_key_pair();
    transcript.worker_ephemeral = bytes_hex(ephemeral.public_key);
    transcript.worker_join_id = credential.worker_join_id;
    transcript.pairing_id = fleet_pairing_id(transcript.coordinator_id, credential.worker_join_id);
    output.write(make_frame(MessageType::join_challenge,
                            nlohmann::json{{"host", host_facts_to_json(host)},
                                           {"worker", endpoint_to_json(self)},
                                           {"worker_ephemeral", transcript.worker_ephemeral},
                                           {"worker_join_id", credential.worker_join_id}}));

    constexpr std::string_view kAccept = "JOIN_ACCEPT.body";
    const nlohmann::json accept = expect_frame(input, MessageType::join_accept);
    if (required_string(accept, "pairing_id", kAccept) != transcript.pairing_id) {
      throw WorkerError(WorkerErrorCode::verification,
                        "JOIN_ACCEPT names a pairing id not derived from this coordinator and "
                        "worker");
    }
    const std::string wrapped_hex = required_string(accept, "member_key_wrapped", kAccept);
    const std::optional<std::vector<std::byte>> wrapped =
        parse_hex_bytes(wrapped_hex, kFleetKeyBytes);
    const std::string signature_hex = required_string(accept, "signature", kAccept);
    const std::optional<std::vector<std::byte>> signature =
        parse_hex_bytes(signature_hex, signature_hex.size() / 2);
    if (!wrapped || !signature) {
      throw WorkerError(WorkerErrorCode::protocol, "JOIN_ACCEPT fields must be lowercase hex");
    }
    const Blake3Digest transcript_hash = transcript.hash();
    if (!ec_verify(credential.token.fleet_public_key,
                   as_bytes(signed_message(transcript_hash, wrapped_hex)), *signature)) {
      throw WorkerError(WorkerErrorCode::verification,
                        "the join was not signed with this fleet's key; refusing to pair");
    }
    const std::vector<std::byte> secret =
        pairing_secret(ec_shared_secret(ephemeral.private_key, coordinator_ephemeral),
                       transcript_hash);
    WorkerJoinResult result;
    result.pairing.key = svp::exec::remote::PairingKey{.pairing_id = transcript.pairing_id,
                                                       .secret = secret};
    result.pairing.created_at = utc_timestamp_now();
    result.member_key = wrap_member_key(*wrapped, secret);
    persist(result);
    output.write(make_frame(MessageType::join_done,
                            nlohmann::json{{"confirm", confirmation(secret, transcript_hash)}}));
    return result;
  });
}

CoordinatorJoinResult join_fleet_worker(FrameReader& input, FrameWriter& output,
                                        const FleetMembership& membership,
                                        std::string_view worker_join_id) {
  return reporting_errors(output, [&] {
    const EcKeyPair ephemeral = generate_ec_key_pair();
    Transcript transcript;
    transcript.coordinator_ephemeral = bytes_hex(ephemeral.public_key);
    transcript.coordinator_id = membership.coordinator_id;
    transcript.fleet_id = membership.fleet.fleet_id;
    output.write(make_frame(MessageType::join_offer,
                            nlohmann::json{{"coordinator_ephemeral",
                                            transcript.coordinator_ephemeral},
                                           {"coordinator_id", transcript.coordinator_id},
                                           {"fleet_id", transcript.fleet_id}}));

    constexpr std::string_view kChallenge = "JOIN_CHALLENGE.body";
    const nlohmann::json challenge = expect_frame(input, MessageType::join_challenge);
    transcript.worker_join_id = required_string(challenge, "worker_join_id", kChallenge);
    if (transcript.worker_join_id != worker_join_id) {
      throw WorkerError(WorkerErrorCode::verification,
                        "the join listener answered as " + transcript.worker_join_id +
                            ", expected " + std::string(worker_join_id));
    }
    const std::vector<std::byte> worker_ephemeral =
        hex_bytes_field(challenge, "worker_ephemeral", kEcPublicKeyBytes, kChallenge);
    transcript.worker_ephemeral = bytes_hex(worker_ephemeral);
    transcript.pairing_id = fleet_pairing_id(transcript.coordinator_id, transcript.worker_join_id);
    CoordinatorJoinResult result;
    result.host = host_facts_from_json(required_object(challenge, "host", kChallenge),
                                       child_path(kChallenge, "host"));
    result.worker = endpoint_from_json(required_object(challenge, "worker", kChallenge),
                                       child_path(kChallenge, "worker"));
    result.worker.join_id = transcript.worker_join_id;
    result.worker.arch = result.host.arch;
    result.worker.os = result.host.os;

    const Blake3Digest transcript_hash = transcript.hash();
    const std::vector<std::byte> secret = pairing_secret(
        ec_shared_secret(ephemeral.private_key, worker_ephemeral), transcript_hash);
    const std::string wrapped_hex = bytes_hex(
        wrap_member_key(member_key(membership.fleet, transcript.worker_join_id), secret));
    const std::vector<std::byte> signature =
        ec_sign(membership.fleet.signing_key, as_bytes(signed_message(transcript_hash, wrapped_hex)));
    output.write(make_frame(MessageType::join_accept,
                            nlohmann::json{{"member_key_wrapped", wrapped_hex},
                                           {"pairing_id", transcript.pairing_id},
                                           {"signature", bytes_hex(signature)}}));

    const nlohmann::json done = expect_frame(input, MessageType::join_done);
    if (required_string(done, "confirm", "JOIN_DONE.body") !=
        confirmation(secret, transcript_hash)) {
      throw WorkerError(WorkerErrorCode::verification,
                        "the worker's join confirmation does not match; not recording the "
                        "pairing");
    }
    result.key = svp::exec::remote::PairingKey{.pairing_id = transcript.pairing_id,
                                               .secret = secret};
    return result;
  });
}

}  // namespace svp::exec::worker
