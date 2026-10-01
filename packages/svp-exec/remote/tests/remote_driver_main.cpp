// svp-exec-remote-driver: coordinator-side driver for cross-machine runs
// against `svp-exec-test-worker --listen` workers. Not a ctest; it needs
// workers on other Macs. Machines are found by pairing id only.
//
//   svp-exec-remote-driver --worker <pairing-id>=<psk-file>[@<slots>] ...
//       [--tasks <per lane>] [--output-bytes <n>] [--sleep-ms <n>]
//       [--local-threads <n>] [--remote-first] [--discover-only]
//
// For every worker: discovery (service, candidate interfaces) and a route
// probe (chosen route, every candidate's handshake, TLS session). Then the
// two-lane toy graph runs in-process as the reference and on the remote
// workers; the driver checks every payload is byte-identical, prints timing
// and payload throughput, retries, and lost/invalid attempts, and exits
// non-zero on any mismatch or failed build.

#include "pairing_test_support.hpp"
#include "scheduler_test_support.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/remote/remote_connector.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/remote_executor.hpp"
#include "svp/exec/scheduler.hpp"

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;
using namespace svp::exec::remote;
using Clock = std::chrono::steady_clock;

struct WorkerArgument {
  PairingKey key;
  std::size_t slots = 1;
};

struct Arguments {
  std::vector<WorkerArgument> workers;
  std::size_t tasks_per_lane = 8;
  std::uint64_t output_bytes = 0;
  std::uint64_t sleep_ms = 0;
  std::size_t local_threads = 0;
  // Run the remote graph before the in-process reference, so the remote run
  // starts at a predictable time (for fault injection from outside).
  bool remote_first = false;
  bool discover_only = false;
};

Arguments parse(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    const auto value = [&]() -> std::string {
      if (index + 1 >= argc) {
        throw std::runtime_error(flag + " needs a value");
      }
      return argv[++index];
    };
    if (flag == "--worker") {
      const std::string spec = value();
      const auto equals = spec.find('=');
      const auto at = spec.find('@');
      if (equals == std::string::npos) {
        throw std::runtime_error("--worker wants <pairing-id>=<psk-file>[@<slots>]");
      }
      WorkerArgument worker;
      const std::string file = spec.substr(equals + 1, at == std::string::npos
                                                           ? std::string::npos
                                                           : at - equals - 1);
      worker.key = svp::exec::remote::test::read_pairing_file(spec.substr(0, equals), file);
      if (at != std::string::npos) {
        worker.slots = std::stoul(spec.substr(at + 1));
      }
      arguments.workers.push_back(std::move(worker));
    } else if (flag == "--tasks") {
      arguments.tasks_per_lane = std::stoul(value());
    } else if (flag == "--output-bytes") {
      arguments.output_bytes = std::stoull(value());
    } else if (flag == "--sleep-ms") {
      arguments.sleep_ms = std::stoull(value());
    } else if (flag == "--local-threads") {
      arguments.local_threads = std::stoul(value());
    } else if (flag == "--remote-first") {
      arguments.remote_first = true;
    } else if (flag == "--discover-only") {
      arguments.discover_only = true;
    } else {
      throw std::runtime_error("unknown argument " + flag);
    }
  }
  if (arguments.workers.empty()) {
    throw std::runtime_error("at least one --worker is required");
  }
  return arguments;
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void probe_worker(const WorkerArgument& worker) {
  std::cout << "== pairing " << worker.key.pairing_id << "\n";
  RemoteConnector connector(RemoteConnectorOptions{.pairing = worker.key});
  auto started = Clock::now();
  for (const DiscoveredWorker& found : connector.discover()) {
    std::cout << "  discovered service `" << found.service_name << "` in "
              << std::fixed << std::setprecision(3) << seconds_since(started) << " s\n";
    for (const RouteCandidate& candidate : found.candidates) {
      std::cout << "    candidate " << candidate.interface_name << " " << candidate.address
                << " medium="
                << route_medium_name(candidate.medium) << " link_bps="
                << candidate.link_rate_bps << "\n";
    }
  }
  started = Clock::now();
  RemoteConnection connection = connector.connect();
  const RouteChoice& route = connection.route;
  std::cout << "  connected in " << seconds_since(started) << " s via "
            << route.route.interface_name << " (" << route_medium_name(route.route.medium)
            << ") to `" << route.service_name << "` peer " << connection.stream->peer_description()
            << "\n";
  const TlsSession tls = connection.stream->tls_session();
  std::cout << "  tls version=0x" << std::hex << tls.protocol_version << " suite=0x"
            << tls.cipher_suite << std::dec << "\n";
  for (const RouteProbe& probe : route.probes) {
    std::cout << "    probe " << probe.candidate.interface_name << " " << probe.candidate.address
              << " "
              << (probe.state == RouteProbeState::ready     ? "ready"
                  : probe.state == RouteProbeState::pending ? "not-needed"
                                                            : "failed")
              << " handshake_us=" << probe.handshake.count() << " up_MBps="
              << probe.upload_bytes_per_second / 1.0e6
              << " down_MBps=" << probe.download_bytes_per_second / 1.0e6
              << (probe.failure.empty() ? "" : " (" + probe.failure + ")") << "\n";
  }
  connection.stream->cancel();
}

class PrintingLog {
 public:
  AttemptObserver observer() {
    return [this](const AttemptEvent& event) {
      if (event.kind == AttemptEventKind::failed || event.kind == AttemptEventKind::expired ||
          event.kind == AttemptEventKind::executor_quarantined) {
        const std::lock_guard lock(mutex_);
        std::cerr << "  [" << std::fixed << std::setprecision(3) << seconds_since(start_)
                  << " s] " << attempt_event_kind_name(event.kind) << " " << event.task_id
                  << " attempt " << event.attempt << " on " << event.executor_id << ": "
                  << event.detail << "\n";
      }
    };
  }

 private:
  std::mutex mutex_;
  Clock::time_point start_ = Clock::now();
};

std::uint64_t payload_bytes(const InMemoryResultCommitSink& sink) {
  std::uint64_t total = 0;
  for (const CommittedResult& committed : sink.results()) {
    for (const FramePayload& payload : committed.payloads) {
      total += payload.size();
    }
  }
  return total;
}

int run_graphs(const Arguments& arguments) {
  std::vector<ToyTask> tasks = two_lane_toy_tasks(arguments.tasks_per_lane);
  for (ToyTask& task : tasks) {
    task.output_bytes = arguments.output_bytes;
    task.sleep_ms = arguments.sleep_ms;
  }
  const TaskGraph graph = make_toy_graph(tasks);
  SchedulerPolicy policy;  // production lease and retry defaults

  std::size_t total_slots = 0;
  for (const WorkerArgument& worker : arguments.workers) {
    total_slots += worker.slots;
  }
  const std::size_t local_threads =
      arguments.local_threads != 0 ? arguments.local_threads : total_slots;

  ToyRuntime runtime;
  InProcessExecutor local(runtime.registry, runtime.store, {.threads = local_threads});
  InMemoryResultCommitSink reference;
  const SteadyClock clock;
  const CancellationToken cancellation;
  double local_seconds = 0.0;
  const auto run_reference = [&] {
    const auto reference_started = Clock::now();
    const std::vector<Executor*> local_executors{&local};
    const BuildOutcome local_outcome =
        Scheduler(policy, clock).run(graph, local_executors, reference, cancellation, {});
    local_seconds = seconds_since(reference_started);
    expect_succeeded(local_outcome, "in-process reference");
  };
  if (!arguments.remote_first) {
    run_reference();
  }

  std::vector<std::unique_ptr<RemoteExecutor>> remotes;
  std::vector<Executor*> executors;
  for (const WorkerArgument& worker : arguments.workers) {
    RemoteExecutorOptions options;
    options.executor_id = "remote:" + worker.key.pairing_id;
    options.connector.pairing = worker.key;
    options.slots = worker.slots;
    remotes.push_back(std::make_unique<RemoteExecutor>(options));
    executors.push_back(remotes.back().get());
  }
  InMemoryResultCommitSink remote_sink;
  PrintingLog log;
  const auto started = Clock::now();
  const BuildOutcome outcome =
      Scheduler(policy, clock).run(graph, executors, remote_sink, cancellation, log.observer());
  const double remote_seconds = seconds_since(started);
  for (const auto& remote : remotes) {
    remote->stop();
  }
  if (arguments.remote_first) {
    run_reference();
  }

  std::cout << "== graph: " << graph.size() << " tasks, output_bytes=" << arguments.output_bytes
            << ", sleep_ms=" << arguments.sleep_ms << "\n";
  std::cout << "  in-process (" << local_threads << " threads): " << std::fixed
            << std::setprecision(3) << local_seconds << " s\n";
  std::cout << "  remote (" << remotes.size() << " workers, " << total_slots
            << " slots): " << remote_seconds << " s, status "
            << build_status_name(outcome.status) << "\n";
  std::cout << "  attempts=" << outcome.stats.attempts_started
            << " retries=" << outcome.stats.retries
            << " expired=" << outcome.stats.leases_expired
            << " invalid=" << outcome.stats.invalid_results << "\n";
  if (outcome.failure) {
    std::cout << "  failure: " << outcome.failure->message << "\n";
  }
  std::map<std::string, std::size_t> per_executor;
  for (const CommittedResult& committed : remote_sink.results()) {
    ++per_executor[committed.executor_id];
  }
  for (const auto& remote : remotes) {
    std::cout << "  " << remote->id() << ": " << per_executor[std::string(remote->id())]
              << " tasks, connections=" << remote->connections_opened();
    if (const auto route = remote->last_route()) {
      std::cout << ", route " << route->route.interface_name << " ("
                << route_medium_name(route->route.medium) << ")";
    }
    std::cout << "\n";
  }
  const std::uint64_t bytes = payload_bytes(remote_sink);
  std::cout << "  result payload bytes=" << bytes << " end-to-end="
            << std::setprecision(1) << (static_cast<double>(bytes) / remote_seconds / 1.0e6)
            << " MB/s\n";

  bool identical = outcome.status == BuildStatus::succeeded;
  for (std::size_t index = 0; identical && index < graph.size(); ++index) {
    const std::string& task_id = graph.node(index).spec.task_id;
    const CommittedResult* want = reference.find(task_id);
    const CommittedResult* got = remote_sink.find(task_id);
    identical = want && got && want->result.output_digest == got->result.output_digest &&
                want->payloads == got->payloads;
  }
  for (const std::string_view lane : {"alpha", "beta"}) {
    if (identical) {
      const std::string want = reduce_lane(graph, lane, reference.results());
      const std::string got = reduce_lane(graph, lane, remote_sink.results());
      identical = want == got;
      std::cout << "  lane " << lane << " reduction " << got << "\n";
    }
  }
  std::cout << "  byte-identical to in-process: " << (identical ? "yes" : "NO") << "\n";
  return identical ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Arguments arguments = parse(argc, argv);
    for (const WorkerArgument& worker : arguments.workers) {
      probe_worker(worker);
    }
    if (arguments.discover_only) {
      return 0;
    }
    return run_graphs(arguments);
  } catch (const std::exception& error) {
    std::cerr << "svp-exec-remote-driver: " << error.what() << "\n";
    return 1;
  }
}
