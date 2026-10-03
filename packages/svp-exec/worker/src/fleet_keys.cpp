#include "svp/exec/worker/fleet_keys.hpp"

#include "hex_bytes.hpp"
#include "message_fields.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/worker/fleet_crypto.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <nlohmann/json.hpp>

namespace svp::exec::worker {
namespace {

using namespace detail;

std::vector<std::byte> derive(const FleetSecret& fleet, std::string_view context,
                              const nlohmann::json& fields) {
  const std::string encoded = encode_canonical_json(fields);
  std::vector<std::byte> material = fleet.secret;
  const auto* begin = reinterpret_cast<const std::byte*>(encoded.data());
  material.insert(material.end(), begin, begin + encoded.size());
  const Blake3Digest key = blake3_derive_key(context, material);
  const auto* key_begin = reinterpret_cast<const std::byte*>(key.data());
  return std::vector<std::byte>(key_begin, key_begin + key.size());
}

[[noreturn]] void bad_token(const std::string& message) {
  throw WorkerError(WorkerErrorCode::configuration, message);
}

nlohmann::json decode_body(std::string_view text, std::string_view prefix, std::string_view kind) {
  if (!text.starts_with(prefix)) {
    bad_token("not " + std::string(kind) + " (it does not start with `" + std::string(prefix) +
              "`)");
  }
  const std::optional<std::string> json = base64url_decode(text.substr(prefix.size()));
  if (!json) {
    bad_token(std::string(kind) + " is not valid base64url (was it copied whole?)");
  }
  try {
    return decode_canonical_json(*json);
  } catch (const std::exception& error) {
    bad_token(std::string(kind) + " is malformed: " + error.what());
  }
}

std::vector<std::byte> hex_field(const nlohmann::json& body, std::string_view name,
                                 std::size_t bytes, std::string_view kind) {
  const std::optional<std::vector<std::byte>> value =
      parse_hex_bytes(required_string(body, name, kind), bytes);
  if (!value) {
    bad_token(std::string(kind) + "." + std::string(name) + " must be " +
              std::to_string(bytes * 2) + " lowercase hex digits");
  }
  return *value;
}

}  // namespace

bool is_fleet_identifier(std::string_view id, std::string_view prefix) noexcept {
  if (!id.starts_with(prefix) || id.size() != prefix.size() + 2 * kFleetIdentifierRandomBytes) {
    return false;
  }
  for (const char c : id.substr(prefix.size())) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string random_fleet_identifier(std::string_view prefix) {
  return std::string(prefix) + bytes_hex(secure_random_bytes(kFleetIdentifierRandomBytes));
}

std::uint64_t utc_seconds_now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

FleetSecret generate_fleet_secret() {
  FleetSecret fleet{.fleet_id = random_fleet_identifier(kFleetIdPrefix),
                    .secret = secure_random_bytes(kFleetKeyBytes),
                    .signing_key = generate_ec_key_pair().private_key};
  validate_fleet_secret(fleet);
  return fleet;
}

void validate_fleet_secret(const FleetSecret& fleet) {
  if (!is_fleet_identifier(fleet.fleet_id, kFleetIdPrefix)) {
    throw WorkerError(WorkerErrorCode::configuration, "fleet id `" + fleet.fleet_id +
                                                          "` is not svpf- and 24 hex digits");
  }
  if (fleet.secret.size() != kFleetKeyBytes) {
    throw WorkerError(WorkerErrorCode::configuration, "the fleet secret must be 256 bits");
  }
  (void)ec_public_key_of(fleet.signing_key);
}

std::vector<std::byte> token_join_key(const FleetSecret& fleet, std::string_view token_id,
                                      std::uint64_t expires_at) {
  return derive(fleet, kJoinTokenKeyContext,
                nlohmann::json{{"expires_at", expires_at}, {"token_id", std::string(token_id)}});
}

std::vector<std::byte> member_key(const FleetSecret& fleet, std::string_view worker_join_id) {
  return derive(fleet, kMemberKeyContext,
                nlohmann::json{{"worker_join_id", std::string(worker_join_id)}});
}

std::string fleet_pairing_id(std::string_view coordinator_id, std::string_view worker_join_id) {
  const std::string encoded =
      encode_canonical_json(nlohmann::json{{"coordinator_id", std::string(coordinator_id)},
                                           {"worker_join_id", std::string(worker_join_id)}});
  const Blake3Digest digest = blake3_derive_key(
      kPairingIdContext, std::as_bytes(std::span(encoded.data(), encoded.size())));
  return std::string(kFleetPairingIdPrefix) +
         blake3_hex(digest).substr(0, 2 * kFleetIdentifierRandomBytes);
}

WorkerJoinToken issue_worker_token(const FleetSecret& fleet, std::uint64_t now,
                                   std::chrono::seconds lifetime) {
  validate_fleet_secret(fleet);
  if (lifetime.count() <= 0 || lifetime > kMaxWorkerTokenLifetime) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "a worker token lives more than 0 and at most " +
                          std::to_string(kMaxWorkerTokenLifetime.count() / 24) + " days");
  }
  WorkerJoinToken token;
  token.fleet_id = fleet.fleet_id;
  token.fleet_public_key = ec_public_key_of(fleet.signing_key);
  token.token_id = bytes_hex(secure_random_bytes(kFleetIdentifierRandomBytes));
  token.expires_at = now + static_cast<std::uint64_t>(lifetime.count());
  token.join_key = token_join_key(fleet, token.token_id, token.expires_at);
  return token;
}

std::string encode_worker_token(const WorkerJoinToken& token) {
  return std::string(kWorkerTokenPrefix) +
         base64url_encode(encode_canonical_json(nlohmann::json{
             {"expires_at", token.expires_at},
             {"fleet_id", token.fleet_id},
             {"fleet_public_key", bytes_hex(token.fleet_public_key)},
             {"join_key", bytes_hex(token.join_key)},
             {"token_id", token.token_id}}));
}

WorkerJoinToken decode_worker_token(std::string_view text) {
  if (text.starts_with(kCoordinatorTokenPrefix)) {
    bad_token("this is a coordinator token (for `workers fleet join`); `worker install --join` "
              "takes a worker token from `workers fleet token`");
  }
  constexpr std::string_view kKind = "worker token";
  const nlohmann::json body = decode_body(text, kWorkerTokenPrefix, kKind);
  try {
    require_object(body, kKind);
    reject_unknown_fields(body, {"expires_at", "fleet_id", "fleet_public_key", "join_key",
                                 "token_id"},
                          kKind);
    WorkerJoinToken token;
    token.fleet_id = required_string(body, "fleet_id", kKind);
    token.expires_at = required_unsigned(body, "expires_at", kKind);
    token.token_id = required_string(body, "token_id", kKind);
    token.fleet_public_key = hex_field(body, "fleet_public_key", kEcPublicKeyBytes, kKind);
    token.join_key = hex_field(body, "join_key", kFleetKeyBytes, kKind);
    if (!is_fleet_identifier(token.fleet_id, kFleetIdPrefix) ||
        !is_fleet_identifier(token.token_id, "")) {
      bad_token("worker token has a malformed fleet or token id");
    }
    return token;
  } catch (const ExecError& error) {
    bad_token(std::string("worker token is malformed: ") + error.what());
  }
}

std::string encode_coordinator_token(const FleetSecret& fleet) {
  validate_fleet_secret(fleet);
  return std::string(kCoordinatorTokenPrefix) +
         base64url_encode(encode_canonical_json(nlohmann::json{
             {"fleet_id", fleet.fleet_id},
             {"fleet_secret", bytes_hex(fleet.secret)},
             {"signing_key", bytes_hex(fleet.signing_key)}}));
}

FleetSecret decode_coordinator_token(std::string_view text) {
  if (text.starts_with(kWorkerTokenPrefix)) {
    bad_token("this is a worker token (for `worker install --join`); `workers fleet join` "
              "takes a coordinator token from `workers fleet token --coordinator`");
  }
  constexpr std::string_view kKind = "coordinator token";
  const nlohmann::json body = decode_body(text, kCoordinatorTokenPrefix, kKind);
  try {
    require_object(body, kKind);
    reject_unknown_fields(body, {"fleet_id", "fleet_secret", "signing_key"}, kKind);
    FleetSecret fleet;
    fleet.fleet_id = required_string(body, "fleet_id", kKind);
    fleet.secret = hex_field(body, "fleet_secret", kFleetKeyBytes, kKind);
    fleet.signing_key = hex_field(body, "signing_key", kEcPrivateKeyBytes, kKind);
    validate_fleet_secret(fleet);
    return fleet;
  } catch (const ExecError& error) {
    bad_token(std::string("coordinator token is malformed: ") + error.what());
  }
}

}  // namespace svp::exec::worker
