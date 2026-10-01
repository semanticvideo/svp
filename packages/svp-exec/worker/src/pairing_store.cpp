#include "svp/exec/worker/pairing_store.hpp"

#include "message_fields.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace svp::exec::worker {
namespace {

using namespace detail;
using svp::exec::remote::PairingKey;

constexpr std::string_view kRecordSuffix = ".json";

std::vector<std::byte> parse_secret(const std::string& text, std::string_view path) {
  if (text.size() != 2 * svp::exec::remote::kMinPairingSecretBytes) {
    throw ExecError(ExecErrorCode::invalid_value,
                    std::string(path) + ".secret must be 64 lowercase hex digits");
  }
  std::vector<std::byte> secret;
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const auto digit = [&](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      throw ExecError(ExecErrorCode::invalid_value,
                      std::string(path) + ".secret must be lowercase hex");
    };
    secret.push_back(static_cast<std::byte>(digit(text[index]) * 16 + digit(text[index + 1])));
  }
  return secret;
}

PairingKey key_from(const nlohmann::json& body, std::string_view path,
                    std::string_view expected_role) {
  if (required_string(body, "schema", path) != kPairingRecordSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    std::string(path) + ".schema must be " + std::string(kPairingRecordSchema));
  }
  if (required_string(body, "role", path) != expected_role) {
    throw ExecError(ExecErrorCode::invalid_value,
                    std::string(path) + ".role must be " + std::string(expected_role));
  }
  PairingKey key{.pairing_id = required_string(body, "pairing_id", path),
                 .secret = parse_secret(required_string(body, "secret", path), path)};
  svp::exec::remote::validate_pairing_key(key);
  return key;
}

std::string mode_problem(const struct stat& info, mode_t allowed) {
  if (info.st_uid != ::geteuid()) {
    return "is owned by another user";
  }
  if ((info.st_mode & 0777 & ~allowed) != 0) {
    return "is accessible by other users (mode must be at most " +
           std::to_string((allowed >> 6) & 7) + std::to_string((allowed >> 3) & 7) +
           std::to_string(allowed & 7) + ")";
  }
  return {};
}

}  // namespace

PairingKey generate_pairing_key() {
  std::vector<std::byte> id_bytes(kPairingIdRandomBytes);
  std::vector<std::byte> secret(svp::exec::remote::kMinPairingSecretBytes);
  if (::getentropy(id_bytes.data(), id_bytes.size()) != 0 ||
      ::getentropy(secret.data(), secret.size()) != 0) {
    throw WorkerError(WorkerErrorCode::io, "the system has no entropy for a pairing secret");
  }
  PairingKey key{.pairing_id = std::string(kPairingIdPrefix) + secret_hex(id_bytes),
                 .secret = std::move(secret)};
  svp::exec::remote::validate_pairing_key(key);
  return key;
}

std::string secret_hex(const std::vector<std::byte>& secret) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(secret.size() * 2);
  for (const std::byte byte : secret) {
    const auto value = static_cast<unsigned>(byte);
    text += kDigits[value >> 4U];
    text += kDigits[value & 0xFU];
  }
  return text;
}

std::string encode_worker_pairing(const WorkerPairingRecord& record) {
  svp::exec::remote::validate_pairing_key(record.key);
  return encode_canonical_json(nlohmann::json{{"created_at", record.created_at},
                                              {"pairing_id", record.key.pairing_id},
                                              {"role", "worker"},
                                              {"schema", std::string(kPairingRecordSchema)},
                                              {"secret", secret_hex(record.key.secret)}});
}

WorkerPairingRecord decode_worker_pairing(std::string_view bytes) {
  const nlohmann::json body = decode_canonical_json(bytes);
  constexpr std::string_view kPath = "pairing";
  require_object(body, kPath);
  reject_unknown_fields(body, {"created_at", "pairing_id", "role", "schema", "secret"}, kPath);
  return WorkerPairingRecord{.key = key_from(body, kPath, "worker"),
                             .created_at = required_string(body, "created_at", kPath)};
}

std::string encode_coordinator_pairing(const CoordinatorPairingRecord& record) {
  svp::exec::remote::validate_pairing_key(record.key);
  const WorkerEndpoint& worker = record.worker;
  return encode_canonical_json(nlohmann::json{
      {"created_at", record.created_at},
      {"pairing_id", record.key.pairing_id},
      {"role", "coordinator"},
      {"runtime", nlohmann::json{{"kind", std::string(runtime_kind_name(record.runtime_kind))},
                                 {"runtime_id", blake3_prefixed(record.runtime_id)}}},
      {"schema", std::string(kPairingRecordSchema)},
      {"secret", secret_hex(record.key.secret)},
      {"worker",
       nlohmann::json{
           {"arch", worker.arch},
           {"home", worker.home},
           {"label", worker.label},
           {"os", nlohmann::json{{"build", worker.os.build},
                                 {"product_version", worker.os.product_version}}},
           {"plist", worker.plist},
           {"root", worker.root},
           {"service_mode", std::string(worker_service_mode_name(worker.service_mode))},
           {"ssh_target", worker.ssh_target},
           {"uid", worker.uid},
           {"user", worker.user},
       }},
  });
}

CoordinatorPairingRecord decode_coordinator_pairing(std::string_view bytes) {
  const nlohmann::json body = decode_canonical_json(bytes);
  constexpr std::string_view kPath = "pairing";
  require_object(body, kPath);
  reject_unknown_fields(
      body, {"created_at", "pairing_id", "role", "runtime", "schema", "secret", "worker"}, kPath);
  CoordinatorPairingRecord record;
  record.key = key_from(body, kPath, "coordinator");
  record.created_at = required_string(body, "created_at", kPath);

  const std::string runtime_path = child_path(kPath, "runtime");
  const nlohmann::json& runtime = required_object(body, "runtime", kPath);
  reject_unknown_fields(runtime, {"kind", "runtime_id"}, runtime_path);
  record.runtime_id = required_blake3_prefixed(runtime, "runtime_id", runtime_path);
  const std::string kind = required_string(runtime, "kind", runtime_path);
  const std::optional<RuntimeKind> runtime_kind = parse_runtime_kind(kind);
  if (!runtime_kind) {
    throw ExecError(ExecErrorCode::invalid_value, runtime_path + ".kind is unknown: " + kind);
  }
  record.runtime_kind = *runtime_kind;

  const std::string worker_path = child_path(kPath, "worker");
  const nlohmann::json& worker = required_object(body, "worker", kPath);
  reject_unknown_fields(worker,
                        {"arch", "home", "label", "os", "plist", "root", "service_mode",
                         "ssh_target", "uid", "user"},
                        worker_path);
  WorkerEndpoint& endpoint = record.worker;
  endpoint.arch = required_string(worker, "arch", worker_path);
  endpoint.home = required_string(worker, "home", worker_path);
  endpoint.label = required_string(worker, "label", worker_path);
  endpoint.plist = required_string(worker, "plist", worker_path);
  endpoint.root = required_string(worker, "root", worker_path);
  endpoint.ssh_target = required_string(worker, "ssh_target", worker_path);
  endpoint.uid = required_u32(worker, "uid", worker_path);
  endpoint.user = required_string(worker, "user", worker_path);
  const std::string mode = required_string(worker, "service_mode", worker_path);
  const std::optional<WorkerServiceMode> service_mode = parse_worker_service_mode(mode);
  if (!service_mode) {
    throw ExecError(ExecErrorCode::invalid_value,
                    worker_path + ".service_mode is unknown: " + mode);
  }
  endpoint.service_mode = *service_mode;
  const std::string os_path = child_path(worker_path, "os");
  const nlohmann::json& os = required_object(worker, "os", worker_path);
  reject_unknown_fields(os, {"build", "product_version"}, os_path);
  endpoint.os.build = required_string(os, "build", os_path);
  endpoint.os.product_version = required_string(os, "product_version", os_path);
  return record;
}

std::string utc_timestamp_now() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  ::gmtime_r(&now, &utc);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

PairingDirectory::PairingDirectory(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::filesystem::path PairingDirectory::file_for(std::string_view pairing_id) const {
  const svp::exec::remote::PairingKey probe{
      .pairing_id = std::string(pairing_id),
      .secret = std::vector<std::byte>(svp::exec::remote::kMinPairingSecretBytes)};
  try {
    svp::exec::remote::validate_pairing_key(probe);
  } catch (const std::exception& error) {
    throw WorkerError(WorkerErrorCode::configuration, error.what());
  }
  return directory_ / (std::string(pairing_id) + std::string(kRecordSuffix));
}

void PairingDirectory::check_directory() const {
  struct stat info{};
  if (::lstat(directory_.c_str(), &info) != 0) {
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "pairing directory " + directory_.string() + " is not a directory");
  }
  if (const std::string problem = mode_problem(info, 0700); !problem.empty()) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "pairing directory " + directory_.string() + " " + problem);
  }
}

void PairingDirectory::write(std::string_view pairing_id, std::string_view bytes) const {
  const std::filesystem::path target = file_for(pairing_id);
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error) {
    throw WorkerError(WorkerErrorCode::io,
                      "cannot create " + directory_.string() + ": " + error.message());
  }
  if (::chmod(directory_.c_str(), 0700) != 0) {
    throw WorkerError(WorkerErrorCode::io, "cannot make " + directory_.string() + " private");
  }
  check_directory();
  const std::filesystem::path temporary =
      target.string() + ".tmp-" + std::to_string(::getpid());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    throw WorkerError(WorkerErrorCode::io, "cannot create " + temporary.string());
  }
  std::size_t written = 0;
  bool ok = ::fchmod(fd, 0600) == 0;
  while (ok && written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    ok = count > 0;
    written += ok ? static_cast<std::size_t>(count) : 0;
  }
  ok = ok && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(temporary.c_str(), target.c_str()) != 0) {
    ::unlink(temporary.c_str());
    throw WorkerError(WorkerErrorCode::io, "cannot write " + target.string());
  }
}

std::optional<std::string> PairingDirectory::read(std::string_view pairing_id) const {
  check_directory();
  const std::filesystem::path file = file_for(pairing_id);
  struct stat info{};
  if (::lstat(file.c_str(), &info) != 0) {
    return std::nullopt;
  }
  if (!S_ISREG(info.st_mode)) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "pairing record " + file.string() + " is not a regular file");
  }
  if (const std::string problem = mode_problem(info, 0600); !problem.empty()) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "pairing record " + file.string() + " " + problem);
  }
  std::ifstream stream(file, std::ios::binary);
  if (!stream) {
    throw WorkerError(WorkerErrorCode::io, "cannot read " + file.string());
  }
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

std::vector<std::string> PairingDirectory::read_all() const {
  std::vector<std::string> ids;
  std::error_code error;
  if (!std::filesystem::exists(directory_, error)) {
    return {};
  }
  check_directory();
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > kRecordSuffix.size() && name.ends_with(kRecordSuffix)) {
      ids.push_back(name.substr(0, name.size() - kRecordSuffix.size()));
    }
  }
  std::sort(ids.begin(), ids.end());
  std::vector<std::string> records;
  for (const std::string& id : ids) {
    if (std::optional<std::string> record = read(id)) {
      records.push_back(std::move(*record));
    }
  }
  return records;
}

bool PairingDirectory::remove(std::string_view pairing_id) const {
  const std::filesystem::path file = file_for(pairing_id);
  std::error_code error;
  return std::filesystem::remove(file, error);
}

std::filesystem::path default_coordinator_pairings_dir() {
  if (const char* override_dir = std::getenv(kPairingsDirEnvironmentVariable.data());
      override_dir != nullptr && override_dir[0] != '\0') {
    return override_dir;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    throw WorkerError(WorkerErrorCode::configuration,
                      "HOME is not set; cannot locate the pairing store");
  }
  return std::filesystem::path(home) / "Library" / "Application Support" / "SVP" / "Pairings";
}

std::vector<CoordinatorPairingRecord> load_coordinator_pairings(
    const PairingDirectory& directory) {
  std::vector<CoordinatorPairingRecord> records;
  for (const std::string& bytes : directory.read_all()) {
    records.push_back(decode_coordinator_pairing(bytes));
  }
  return records;
}

std::vector<WorkerPairingRecord> load_worker_pairings(const PairingDirectory& directory) {
  std::vector<WorkerPairingRecord> records;
  for (const std::string& bytes : directory.read_all()) {
    records.push_back(decode_worker_pairing(bytes));
  }
  return records;
}

}  // namespace svp::exec::worker
