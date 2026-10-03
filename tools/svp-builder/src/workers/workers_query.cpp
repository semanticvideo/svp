// `svp-builder workers list|status|sync`: reach paired workers by pairing id
// (never by address, plan §3.4) and report or update what they hold.

#include "coordinator_context.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "ocr_calibration_runs.hpp"
#include "worker_reach.hpp"
#include "worker_restart.hpp"
#include "workers_cli.hpp"

#include <future>
#include <iostream>
#include <nlohmann/json.hpp>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;

struct Row {
  CoordinatorPairingRecord record;
  WorkerReach reach;
};

std::vector<Row> reach_all(const std::vector<CoordinatorPairingRecord>& records,
                           const CoordinatorContext& context) {
  std::vector<std::future<WorkerReach>> pending;
  for (const CoordinatorPairingRecord& record : records) {
    pending.push_back(std::async(std::launch::async,
                                 [&record, &context] { return reach_worker(record, context); }));
  }
  std::vector<Row> rows;
  for (std::size_t index = 0; index < records.size(); ++index) {
    rows.push_back(Row{.record = records[index], .reach = pending[index].get()});
  }
  return rows;
}

nlohmann::json row_json(const Row& row, const CoordinatorContext& context) {
  const CoordinatorPairingRecord& record = row.record;
  nlohmann::json value{
      {"pairing_id", record.key.pairing_id},
      {"ssh_target", record.worker.ssh_target},
      {"service_mode", std::string(worker_service_mode_name(record.worker.service_mode))},
      {"root", record.worker.root},
      {"paired_at", record.created_at},
      {"paired_runtime_id", svp::exec::blake3_prefixed(record.runtime_id)},
      {"paired_runtime_kind", std::string(runtime_kind_name(record.runtime_kind))},
      {"reachable", row.reach.reachable},
  };
  if (!row.reach.reachable) {
    value["error"] = row.reach.error;
    return value;
  }
  const WorkerHelloAck& ack = *row.reach.ack;
  nlohmann::json runtimes = nlohmann::json::array();
  for (const auto& runtime : ack.runtimes) {
    runtimes.push_back(svp::exec::blake3_prefixed(runtime));
  }
  nlohmann::json bundles = nlohmann::json::array();
  for (const auto& bundle : ack.model_bundles) {
    bundles.push_back(svp::exec::blake3_hex(bundle));
  }
  value["route"] = nlohmann::json{
      {"interface", row.reach.route->route.interface_name},
      {"medium", std::string(svp::exec::remote::route_medium_name(row.reach.route->route.medium))},
      {"service", row.reach.route->service_name}};
  value["worker"] = nlohmann::json{
      {"host", host_facts_to_json(ack.host)},
      {"memory_available_bytes", ack.memory.available_bytes},
      {"memory_pressure", std::string(memory_pressure_name(ack.memory.pressure))},
      {"memory_reserve_bytes", ack.memory_reserve_bytes},
      {"disk_available_bytes", ack.disk_available_bytes},
      {"cache_blob_count", ack.cache_blob_count},
      {"cache_total_bytes", ack.cache_total_bytes},
      {"runtimes", runtimes},
      {"model_bundles", bundles},
      {"active_sessions", ack.active_sessions},
      {"agent_runtime_id",
       ack.agent_runtime_id ? svp::exec::blake3_prefixed(*ack.agent_runtime_id) : ""},
  };
  value["coordinator_runtime_id"] = svp::exec::blake3_prefixed(context.runtime.runtime_id);
  value["coordinator_runtime_present"] = ack.runtime_present;
  return value;
}

void print_row(const Row& row, const CoordinatorContext& context, bool detailed) {
  const CoordinatorPairingRecord& record = row.record;
  std::cout << record.key.pairing_id << "  " << record.worker.ssh_target << "  "
            << worker_service_mode_name(record.worker.service_mode) << "  ";
  if (!row.reach.reachable) {
    std::cout << "unreachable: " << row.reach.error << "\n";
    return;
  }
  const WorkerHelloAck& ack = *row.reach.ack;
  std::cout << "reachable via " << row.reach.route->route.interface_name << " ("
            << svp::exec::remote::route_medium_name(row.reach.route->route.medium) << ")  macOS "
            << ack.host.os.product_version << " (" << ack.host.os.build << ")  "
            << ack.host.logical_cpus << " cpus  " << format_bytes(ack.host.physical_memory_bytes)
            << "  runtime " << (ack.runtime_present ? "current" : "missing") << "\n";
  if (!detailed) {
    return;
  }
  std::cout << "  service       " << row.reach.route->service_name << "\n"
            << "  cpu           " << ack.host.cpu_brand << ", " << ack.host.performance_cpus
            << " performance + " << ack.host.efficiency_cpus << " efficiency\n"
            << "  memory        " << format_bytes(ack.memory.available_bytes) << " available, "
            << format_bytes(ack.memory_reserve_bytes) << " reserve, pressure "
            << memory_pressure_name(ack.memory.pressure) << "\n"
            << "  disk          " << format_bytes(ack.disk_available_bytes) << " available\n"
            << "  cache         " << ack.cache_blob_count << " blobs, "
            << format_bytes(ack.cache_total_bytes) << "\n"
            << "  sessions      " << ack.active_sessions << " active (including this one)\n"
            << "  agent runtime "
            << (ack.agent_runtime_id ? svp::exec::blake3_prefixed(*ack.agent_runtime_id)
                                     : std::string("unknown"))
            << "\n";
  for (const auto& runtime : ack.runtimes) {
    std::cout << "  runtime       " << svp::exec::blake3_prefixed(runtime)
              << (runtime == context.runtime.runtime_id ? "  (this coordinator's)" : "") << "\n";
  }
  for (const auto& bundle : ack.model_bundles) {
    std::cout << "  model bundle  blake3:" << svp::exec::blake3_hex(bundle) << "\n";
  }
}

}  // namespace

int run_workers_list(const WorkersCliOptions& options) {
  const PairingDirectory store(default_coordinator_pairings_dir());
  const std::vector<CoordinatorPairingRecord> records = load_coordinator_pairings(store);
  if (records.empty() && !options.json) {
    std::cout << "no paired workers (pair one with `svp-builder workers pair <user>@<host>`)\n";
    return 0;
  }
  const CoordinatorContext context = load_coordinator_context();
  const std::vector<Row> rows = reach_all(records, context);
  if (options.json) {
    nlohmann::json array = nlohmann::json::array();
    for (const Row& row : rows) {
      array.push_back(row_json(row, context));
    }
    std::cout << array.dump(2) << "\n";
    return 0;
  }
  for (const Row& row : rows) {
    print_row(row, context, false);
  }
  return 0;
}

int run_workers_status(const WorkersCliOptions& options) {
  const PairingDirectory store(default_coordinator_pairings_dir());
  std::vector<CoordinatorPairingRecord> records;
  if (options.worker.empty()) {
    records = load_coordinator_pairings(store);
  } else {
    records.push_back(find_pairing(store, options.worker));
  }
  const CoordinatorContext context = load_coordinator_context();
  const std::vector<Row> rows = reach_all(records, context);
  bool all_reachable = true;
  nlohmann::json array = nlohmann::json::array();
  for (const Row& row : rows) {
    all_reachable = all_reachable && row.reach.reachable;
    if (options.json) {
      array.push_back(row_json(row, context));
    } else {
      print_row(row, context, true);
    }
  }
  if (options.json) {
    std::cout << array.dump(2) << "\n";
  }
  return all_reachable ? 0 : 1;
}

int run_workers_sync(const WorkersCliOptions& options) {
  const PairingDirectory store(default_coordinator_pairings_dir());
  const CoordinatorPairingRecord record = find_pairing(store, options.worker);
  const CoordinatorContext context = load_coordinator_context();
  const std::vector<ModelBundleSource> models = select_model_bundles(context, options.models);
  const std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
  WorkerSessionClient client(*connection->reader, *connection->writer);
  const WorkerHelloAck ack = client.hello(context.hello());
  TransferStats stats;
  client.ensure_runtime(context.runtime, stats);
  std::cout << "runtime " << svp::exec::blake3_prefixed(context.runtime.runtime_id) << ": "
            << (stats.runtime_pushed ? "pushed and verified" : "already present") << "\n";
  const std::uint64_t runtime_bytes_sent = stats.bytes_sent;
  client.ensure_model_bundles(models, stats);
  client.shutdown();
  // A runtime the worker's service will move to restarts it right after this
  // session; calibration's sessions must not find it gone.
  (void)await_worker_runtime_switch(record.key, context.hello(), context.runtime, ack, nullptr,
                                    [](const std::string& line) { std::cout << line << "\n" << std::flush; });
  for (const std::string& bundle : stats.model_bundles_pushed) {
    std::cout << "model bundle " << bundle << ": pushed and verified against model-lock\n";
  }
  std::cout << models.size() - stats.model_bundles_pushed.size()
            << " model bundle(s) already present; sent " << stats.blobs_sent << " blob(s), "
            << format_bytes(stats.bytes_sent) << " (runtime " << format_bytes(runtime_bytes_sent)
            << ")\n";
  return calibrate_for_workers_command(record, context.hello(), context.runtime,
                                       context.model_cache)
             ? 0
             : 1;
}

}  // namespace svp::builder::workers
