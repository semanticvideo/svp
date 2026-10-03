#include "svp/exec/worker/worker_identity.hpp"

#include "message_fields.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/worker/fleet_keys.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_error.hpp"

namespace svp::exec::worker {
namespace {

// The record's name inside the root's PairingDirectory (<name>.json).
constexpr std::string_view kWorkerIdRecordName = "worker-id";

}  // namespace

std::string load_or_create_worker_id(const WorkerLayout& layout) {
  const PairingDirectory directory(layout.root);
  if (const std::optional<std::string> bytes = directory.read(kWorkerIdRecordName)) {
    try {
      const nlohmann::json body = decode_canonical_json(*bytes);
      const std::string id = detail::required_string(body, "worker_id", "worker_id_record");
      if (detail::required_string(body, "schema", "worker_id_record") == kWorkerIdSchema &&
          is_fleet_identifier(id, kWorkerIdPrefix)) {
        return id;
      }
    } catch (const std::exception&) {
      // Replaced below.
    }
  }
  const std::string id = random_fleet_identifier(kWorkerIdPrefix);
  directory.write(kWorkerIdRecordName,
                  encode_canonical_json(nlohmann::json{{"schema", std::string(kWorkerIdSchema)},
                                                       {"worker_id", id}}));
  return id;
}

}  // namespace svp::exec::worker
