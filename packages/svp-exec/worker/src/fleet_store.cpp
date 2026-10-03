#include "svp/exec/worker/fleet_store.hpp"

#include "hex_bytes.hpp"
#include "message_fields.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/worker/fleet_crypto.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace svp::exec::worker {
namespace {

using namespace detail;

// The record names inside their PairingDirectory (<name>.json).
constexpr std::string_view kFleetRecordName = "fleet";
constexpr std::string_view kJoinRecordName = "join";

std::vector<std::byte> hex_field(const nlohmann::json& body, std::string_view name,
                                 std::size_t bytes, std::string_view path) {
  const std::optional<std::vector<std::byte>> value =
      parse_hex_bytes(required_string(body, name, path), bytes);
  if (!value) {
    throw ExecError(ExecErrorCode::invalid_value, std::string(path) + "." + std::string(name) +
                                                      " must be " + std::to_string(bytes * 2) +
                                                      " lowercase hex digits");
  }
  return *value;
}

template <typename Function>
auto as_configuration_error(std::string_view what, Function&& function) {
  try {
    return function();
  } catch (const ExecError& error) {
    throw WorkerError(WorkerErrorCode::configuration,
                      std::string(what) + " is malformed: " + error.what());
  }
}

}  // namespace

std::string encode_fleet_membership(const FleetMembership& membership) {
  validate_fleet_secret(membership.fleet);
  return encode_canonical_json(nlohmann::json{
      {"coordinator_id", membership.coordinator_id},
      {"created_at", membership.created_at},
      {"fleet_id", membership.fleet.fleet_id},
      {"fleet_secret", bytes_hex(membership.fleet.secret)},
      {"schema", std::string(kFleetRecordSchema)},
      {"signing_key", bytes_hex(membership.fleet.signing_key)}});
}

FleetMembership decode_fleet_membership(std::string_view bytes) {
  constexpr std::string_view kPath = "fleet";
  FleetMembership membership = as_configuration_error("the fleet record", [&] {
    const nlohmann::json body = decode_canonical_json(bytes);
    require_object(body, kPath);
    reject_unknown_fields(body, {"coordinator_id", "created_at", "fleet_id", "fleet_secret",
                                 "schema", "signing_key"},
                          kPath);
    if (required_string(body, "schema", kPath) != kFleetRecordSchema) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "fleet.schema must be " + std::string(kFleetRecordSchema));
    }
    FleetMembership decoded;
    decoded.coordinator_id = required_string(body, "coordinator_id", kPath);
    decoded.created_at = required_string(body, "created_at", kPath);
    decoded.fleet.fleet_id = required_string(body, "fleet_id", kPath);
    decoded.fleet.secret = hex_field(body, "fleet_secret", kFleetKeyBytes, kPath);
    decoded.fleet.signing_key = hex_field(body, "signing_key", kEcPrivateKeyBytes, kPath);
    return decoded;
  });
  validate_fleet_secret(membership.fleet);
  if (!is_fleet_identifier(membership.coordinator_id, kCoordinatorIdPrefix)) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "the fleet record's coordinator_id is not svpc- and 24 hex digits");
  }
  return membership;
}

std::filesystem::path default_fleet_dir() {
  if (const char* override_dir = std::getenv(kFleetDirEnvironmentVariable.data());
      override_dir != nullptr && override_dir[0] != '\0') {
    return override_dir;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    throw WorkerError(WorkerErrorCode::configuration,
                      "HOME is not set; cannot locate the fleet record");
  }
  return std::filesystem::path(home) / "Library" / "Application Support" / "SVP" / "Fleet";
}

std::optional<FleetMembership> load_fleet_membership(const std::filesystem::path& directory) {
  const std::optional<std::string> bytes = PairingDirectory(directory).read(kFleetRecordName);
  if (!bytes) {
    return std::nullopt;
  }
  return decode_fleet_membership(*bytes);
}

void save_fleet_membership(const std::filesystem::path& directory,
                           const FleetMembership& membership) {
  PairingDirectory(directory).write(kFleetRecordName, encode_fleet_membership(membership));
}

std::string encode_worker_join_credential(const WorkerJoinCredential& credential) {
  nlohmann::json body{
      {"created_at", credential.created_at},
      {"fleet_id", credential.token.fleet_id},
      {"fleet_public_key", bytes_hex(credential.token.fleet_public_key)},
      {"schema", std::string(kWorkerJoinSchema)},
      {"token", nlohmann::json{{"expires_at", credential.token.expires_at},
                               {"join_key", bytes_hex(credential.token.join_key)},
                               {"token_id", credential.token.token_id}}},
      {"worker_join_id", credential.worker_join_id}};
  if (credential.member_key) {
    body["member_key"] = bytes_hex(*credential.member_key);
  }
  return encode_canonical_json(body);
}

WorkerJoinCredential decode_worker_join_credential(std::string_view bytes) {
  constexpr std::string_view kPath = "join";
  WorkerJoinCredential credential = as_configuration_error("the join credential", [&] {
    const nlohmann::json body = decode_canonical_json(bytes);
    require_object(body, kPath);
    reject_unknown_fields(body, {"created_at", "fleet_id", "fleet_public_key", "member_key",
                                 "schema", "token", "worker_join_id"},
                          kPath);
    if (required_string(body, "schema", kPath) != kWorkerJoinSchema) {
      throw ExecError(ExecErrorCode::invalid_value,
                      "join.schema must be " + std::string(kWorkerJoinSchema));
    }
    WorkerJoinCredential decoded;
    decoded.created_at = required_string(body, "created_at", kPath);
    decoded.worker_join_id = required_string(body, "worker_join_id", kPath);
    decoded.token.fleet_id = required_string(body, "fleet_id", kPath);
    decoded.token.fleet_public_key =
        hex_field(body, "fleet_public_key", kEcPublicKeyBytes, kPath);
    const std::string token_path = child_path(kPath, "token");
    const nlohmann::json& token = required_object(body, "token", kPath);
    reject_unknown_fields(token, {"expires_at", "join_key", "token_id"}, token_path);
    decoded.token.expires_at = required_unsigned(token, "expires_at", token_path);
    decoded.token.join_key = hex_field(token, "join_key", kFleetKeyBytes, token_path);
    decoded.token.token_id = required_string(token, "token_id", token_path);
    if (body.contains("member_key")) {
      decoded.member_key = hex_field(body, "member_key", kFleetKeyBytes, kPath);
    }
    return decoded;
  });
  if (!is_fleet_identifier(credential.worker_join_id, kWorkerJoinIdPrefix) ||
      !is_fleet_identifier(credential.token.fleet_id, kFleetIdPrefix) ||
      !is_fleet_identifier(credential.token.token_id, "")) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "the join credential has a malformed worker, fleet, or token id");
  }
  return credential;
}

std::optional<WorkerJoinCredential> load_worker_join_credential(const WorkerLayout& layout) {
  const std::optional<std::string> bytes = PairingDirectory(layout.root).read(kJoinRecordName);
  if (!bytes) {
    return std::nullopt;
  }
  return decode_worker_join_credential(*bytes);
}

void save_worker_join_credential(const WorkerLayout& layout,
                                 const WorkerJoinCredential& credential) {
  PairingDirectory(layout.root).write(kJoinRecordName, encode_worker_join_credential(credential));
}

svp::exec::remote::PairingKey join_listener_key(const WorkerJoinCredential& credential) {
  return svp::exec::remote::PairingKey{
      .pairing_id = credential.worker_join_id,
      .secret = credential.member_key ? *credential.member_key : credential.token.join_key};
}

}  // namespace svp::exec::worker
