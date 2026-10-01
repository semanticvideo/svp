// svp-exec-worker-driver: runs the svp-exec toy graph on a worker paired
// with `svp-builder workers pair`, through its launchd agent. Not a ctest:
// it needs a real paired worker. The worker is found by pairing id only.
//
//   svp-exec-worker-driver --pairing <pairing-id> [--pairings-dir <dir>]
//       [--session-program <svp-exec-test-worker>] [--tasks <per lane>]
//       [--slots <n>] [--output-bytes <n>]
//
// The coordinator's real runtime has no toy task type, so the driver ships
// a stand-in runtime whose session program (bin/svp-builder) is
// svp-exec-test-worker; it implements the same session-program contract
// (session_process.hpp). Each RemoteExecutor session runs the worker
// handshake as its preamble: HELLO, then RUNTIME_HAVE and, the first time,
// the runtime's blobs and RUNTIME_PUT. The graph's results are compared
// byte for byte with an in-process run of the same graph.

#include "scheduler_test_support.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/runtime_source.hpp"

#include <csignal>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;
using namespace svp::exec::worker;
using Clock = std::chrono::steady_clock;

struct Arguments {
  std::string pairing_id;
  std::filesystem::path pairings_dir = default_coordinator_pairings_dir();
  std::filesystem::path session_program = SVP_EXEC_TEST_WORKER_PATH;
  std::size_t tasks_per_lane = 8;
  std::size_t slots = 2;
  std::uint64_t output_bytes = 0;
};

Arguments parse(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    if (index + 1 >= argc) {
      throw std::runtime_error(flag + " needs a value");
    }
    const std::string value = argv[++index];
    if (flag == "--pairing") {
      arguments.pairing_id = value;
    } else if (flag == "--pairings-dir") {
      arguments.pairings_dir = value;
    } else if (flag == "--session-program") {
      arguments.session_program = value;
    } else if (flag == "--tasks") {
      arguments.tasks_per_lane = std::stoul(value);
    } else if (flag == "--slots") {
      arguments.slots = std::stoul(value);
    } else if (flag == "--output-bytes") {
      arguments.output_bytes = std::stoull(value);
    } else {
      throw std::runtime_error("unknown argument " + flag);
    }
  }
  if (arguments.pairing_id.empty()) {
    throw std::runtime_error("--pairing is required");
  }
  return arguments;
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void print_ack(const WorkerHelloAck& ack) {
  std::cout << "  worker: macOS " << ack.host.os.product_version << " (" << ack.host.os.build
            << ") " << ack.host.arch << ", " << ack.host.logical_cpus << " cpus ("
            << ack.host.performance_cpus << "P+" << ack.host.efficiency_cpus << "E), "
            << ack.host.physical_memory_bytes / (1ULL << 20) << " MiB, available "
            << ack.memory.available_bytes / (1ULL << 20) << " MiB, reserve "
            << ack.memory_reserve_bytes / (1ULL << 20) << " MiB, pressure "
            << memory_pressure_name(ack.memory.pressure) << "\n";
  std::cout << "  worker runtimes:";
  for (const Blake3Digest& runtime : ack.runtimes) {
    std::cout << " " << blake3_prefixed(runtime);
  }
  std::cout << "\n  worker agent runtime: "
            << (ack.agent_runtime_id ? blake3_prefixed(*ack.agent_runtime_id) : "unknown") << "\n";
}

int run(const Arguments& arguments) {
  const PairingDirectory store(arguments.pairings_dir);
  const std::optional<std::string> bytes = store.read(arguments.pairing_id);
  if (!bytes) {
    throw std::runtime_error("no pairing " + arguments.pairing_id + " in " +
                             arguments.pairings_dir.string());
  }
  const CoordinatorPairingRecord pairing = decode_coordinator_pairing(*bytes);
  const CoordinatorRuntime runtime = single_program_runtime(
      std::filesystem::weakly_canonical(arguments.session_program), "svp-exec-test-worker",
      "stand-in runtime for the toy graph");
  std::cout << "== pairing " << pairing.key.pairing_id << " (" << pairing.worker.ssh_target
            << ", " << worker_service_mode_name(pairing.worker.service_mode) << ")\n";
  std::cout << "  toy runtime_id " << blake3_prefixed(runtime.runtime_id) << " ("
            << runtime.files.front().blob.ref.bytes << " bytes session program)\n";

  const CoordinatorHello hello = make_coordinator_hello(
      runtime,
      svp::models::resolve_local_thread_plan(svp::models::detect_host_cpu_topology(), 1),
      std::nullopt);
  std::mutex mutex;
  std::size_t preambles = 0;
  TransferStats transfer;
  const auto preamble = [&](FrameReader& reader, FrameWriter& writer) {
    WorkerSessionClient client(reader, writer);
    const auto started = Clock::now();
    const WorkerHelloAck ack = client.hello(hello);
    TransferStats stats;
    client.ensure_runtime(runtime, stats);
    const std::lock_guard lock(mutex);
    if (preambles++ == 0) {
      print_ack(ack);
    }
    std::cout << "  session preamble " << preambles << ": runtime_present=" << ack.runtime_present
              << " pushed=" << stats.runtime_pushed << " blobs_sent=" << stats.blobs_sent
              << " bytes_sent=" << stats.bytes_sent << " in " << std::fixed << std::setprecision(3)
              << seconds_since(started) << " s\n";
    transfer.blobs_sent += stats.blobs_sent;
    transfer.bytes_sent += stats.bytes_sent;
    transfer.runtime_pushed = transfer.runtime_pushed || stats.runtime_pushed;
  };

  std::vector<ToyTask> tasks = two_lane_toy_tasks(arguments.tasks_per_lane);
  for (ToyTask& task : tasks) {
    task.output_bytes = arguments.output_bytes;
  }
  const TaskGraph graph = make_toy_graph(tasks);
  SchedulerPolicy policy;
  const SteadyClock clock;
  const CancellationToken cancellation;

  ToyRuntime local_runtime;
  InProcessExecutor local(local_runtime.registry, local_runtime.store,
                          {.threads = arguments.slots});
  InMemoryResultCommitSink reference;
  const std::vector<Executor*> local_executors{&local};
  expect_succeeded(Scheduler(policy, clock).run(graph, local_executors, reference, cancellation, {}),
                   "in-process reference");

  remote::RemoteExecutorOptions options;
  options.executor_id = "remote:" + pairing.key.pairing_id;
  options.connector.pairing = pairing.key;
  options.slots = arguments.slots;
  options.session_preamble = preamble;
  remote::RemoteExecutor executor(options);
  InMemoryResultCommitSink remote_sink;
  const std::vector<Executor*> executors{&executor};
  const auto started = Clock::now();
  const BuildOutcome outcome =
      Scheduler(policy, clock).run(graph, executors, remote_sink, cancellation, {});
  const double seconds = seconds_since(started);
  executor.stop();

  std::cout << "== graph: " << graph.size() << " toy tasks, " << arguments.slots
            << " slots: status " << build_status_name(outcome.status) << " in " << std::fixed
            << std::setprecision(3) << seconds << " s, attempts=" << outcome.stats.attempts_started
            << " retries=" << outcome.stats.retries << "\n";
  if (outcome.failure) {
    std::cout << "  failure: " << outcome.failure->message << "\n";
  }
  if (const auto route = executor.last_route()) {
    std::cout << "  route " << route->route.interface_name << " ("
              << remote::route_medium_name(route->route.medium) << ") to `"
              << route->service_name << "`\n";
  }
  std::cout << "  runtime pushed=" << transfer.runtime_pushed << " blobs=" << transfer.blobs_sent
            << " bytes=" << transfer.bytes_sent << "\n";
  bool identical = outcome.status == BuildStatus::succeeded;
  bool stamped = true;
  for (std::size_t index = 0; identical && index < graph.size(); ++index) {
    const std::string& task_id = graph.node(index).spec.task_id;
    const CommittedResult* want = reference.find(task_id);
    const CommittedResult* got = remote_sink.find(task_id);
    identical = want && got && want->result.output_digest == got->result.output_digest &&
                want->payloads == got->payloads;
    stamped = stamped && got && got->result.execution.runtime_id == runtime.runtime_id;
  }
  for (const std::string_view lane : {"alpha", "beta"}) {
    if (identical) {
      identical = reduce_lane(graph, lane, reference.results()) ==
                  reduce_lane(graph, lane, remote_sink.results());
    }
  }
  std::cout << "  results stamped with the pushed runtime_id: " << (stamped ? "yes" : "NO") << "\n";
  std::cout << "  byte-identical to in-process: " << (identical ? "yes" : "NO") << "\n";
  return identical && stamped ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    return run(parse(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "svp-exec-worker-driver: " << error.what() << "\n";
    return 1;
  }
}
