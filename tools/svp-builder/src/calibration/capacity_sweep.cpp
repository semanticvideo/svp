#include "calibration/capacity_sweep.hpp"

#include "svp/exec/clock.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::builder::calibration {
namespace {

constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;
constexpr std::string_view kCalibrationLane = "calibration";

std::string task_id(const std::string& name, std::size_t step, std::size_t chain,
                    std::size_t batch) {
  return "task.calibration." + name + ".slots_" + std::to_string(step) + ".chain_" +
         std::to_string(chain) + ".batch_" + std::to_string(batch);
}

std::string warm_up_task_id(const std::string& name, std::size_t step, std::size_t chain) {
  return "task.calibration." + name + ".slots_" + std::to_string(step) + ".warm_up_" +
         std::to_string(chain);
}

struct StepTiming {
  std::size_t tasks = 0;
  std::size_t committed = 0;
  std::chrono::milliseconds first_lease{-1};
  std::chrono::milliseconds last_commit{0};
};

}  // namespace

nlohmann::json capacity_calibration_to_json(const CapacityCalibration& calibration) {
  nlohmann::json sweep = nlohmann::json::array();
  for (const CapacityStep& step : calibration.sweep) {
    sweep.push_back({{"items", step.items},
                     {"items_per_second", step.items_per_second},
                     {"slots", step.slots},
                     {"wall_ms", step.wall_ms}});
  }
  return {{"seconds_per_item", calibration.seconds_per_item},
          {"slots", calibration.slots},
          {"stopped_because", calibration.stopped_because},
          {"sweep", std::move(sweep)}};
}

CapacityCalibration capacity_calibration_from_json(const nlohmann::json& value) {
  CapacityCalibration calibration;
  calibration.slots = value.at("slots").get<std::size_t>();
  calibration.seconds_per_item = value.at("seconds_per_item").get<double>();
  calibration.stopped_because = value.at("stopped_because").get<std::string>();
  for (const nlohmann::json& step : value.at("sweep")) {
    calibration.sweep.push_back(CapacityStep{
        .slots = step.at("slots").get<std::size_t>(),
        .items = step.at("items").get<std::uint64_t>(),
        .wall_ms = step.at("wall_ms").get<std::int64_t>(),
        .items_per_second = step.at("items_per_second").get<double>()});
  }
  if (calibration.slots == 0 || !(calibration.seconds_per_item > 0.0)) {
    throw std::invalid_argument("calibration needs slots >= 1 and a positive item time");
  }
  return calibration;
}

std::size_t capacity_max_slots(std::uint64_t available_bytes, std::uint64_t reserve_bytes,
                               std::uint32_t logical_cpus, std::uint64_t peak_rss_mb) {
  const std::uint64_t usable = available_bytes > reserve_bytes ? available_bytes - reserve_bytes : 0;
  const std::uint64_t per_slot = std::max<std::uint64_t>(1, peak_rss_mb) * kBytesPerMiB;
  const std::uint64_t by_memory = usable / per_slot;
  const std::uint64_t by_cpu = std::max<std::uint32_t>(1, logical_cpus);
  return static_cast<std::size_t>(std::max<std::uint64_t>(1, std::min(by_memory, by_cpu)));
}

CapacityCalibrationGraph capacity_calibration_graph(const CapacityWorkload& workload,
                                                    std::size_t max_slots) {
  // Step k first runs k concurrent warm-up tasks (untimed), so each of its k
  // slots holds a loaded model before timing starts; then k timed chains.
  // Without the warm-up, loading the k-th model would count against step k
  // and bias the sweep toward fewer slots.
  CapacityCalibrationGraph graph;
  std::vector<std::string> previous_step;
  for (std::size_t step = 1; step <= max_slots; ++step) {
    std::vector<std::string> warmed;
    for (std::size_t chain = 0; chain < step; ++chain) {
      svp::exec::TaskSpec spec = workload.warm_up;
      spec.task_id = warm_up_task_id(workload.name, step, chain);
      spec.depends_on = previous_step;
      std::sort(spec.depends_on.begin(), spec.depends_on.end());
      warmed.push_back(spec.task_id);
      graph.nodes.push_back(svp::exec::TaskNode{
          .spec = std::move(spec),
          .order_key = {.lane = std::string(kCalibrationLane), .ordinals = {step, 0, chain}}});
    }
    std::sort(warmed.begin(), warmed.end());
    std::vector<std::string> this_step;
    for (std::size_t chain = 0; chain < step; ++chain) {
      for (std::size_t index = 0; index < kCapacityBatchesPerSlot; ++index) {
        svp::exec::TaskSpec spec = workload.batch;
        spec.task_id = task_id(workload.name, step, chain, index);
        spec.depends_on =
            index == 0 ? warmed
                       : std::vector<std::string>{task_id(workload.name, step, chain, index - 1)};
        graph.timed_step[spec.task_id] = step;
        graph.nodes.push_back(svp::exec::TaskNode{
            .spec = std::move(spec),
            .order_key = {.lane = std::string(kCalibrationLane),
                          .ordinals = {step, 1, chain, index}}});
      }
      this_step.push_back(task_id(workload.name, step, chain, kCapacityBatchesPerSlot - 1));
    }
    std::sort(this_step.begin(), this_step.end());
    previous_step = std::move(this_step);
  }
  return graph;
}

CapacityCalibration calibrate_capacity(svp::exec::Executor& executor, std::size_t max_slots,
                                       const CapacityWorkload& workload,
                                       const svp::exec::CancellationToken& cancellation) {
  if (max_slots == 0 || executor.slots() < max_slots) {
    throw std::invalid_argument("calibration executor has fewer slots than the sweep needs");
  }
  if (workload.items_per_batch == 0) {
    throw std::invalid_argument("calibration workload has no items");
  }

  CapacityCalibrationGraph plan = capacity_calibration_graph(workload, max_slots);
  const std::map<std::string, std::size_t>& step_of = plan.timed_step;
  const svp::exec::TaskGraph graph(std::move(plan.nodes));

  std::vector<StepTiming> steps(max_slots + 1);
  for (const auto& [id, step] : step_of) {
    ++steps[step].tasks;
  }
  CapacityCalibration result;
  double best = 0.0;
  std::size_t best_slots = 1;
  double best_seconds_per_item = 0.0;
  svp::exec::CancellationToken stop;
  std::string interrupted;
  const svp::exec::SteadyClock clock;
  const svp::exec::AttemptObserver observer = [&](const svp::exec::AttemptEvent& event) {
    const auto found = step_of.find(event.task_id);
    if (found == step_of.end()) {
      return;
    }
    StepTiming& timing = steps[found->second];
    if (event.kind == svp::exec::AttemptEventKind::failed ||
        event.kind == svp::exec::AttemptEventKind::expired ||
        event.kind == svp::exec::AttemptEventKind::deadline_exceeded) {
      // A step whose attempt was lost or retried would time the retry, not
      // the Mac: the measurement is void.
      interrupted = event.task_id + " attempt " + std::to_string(event.attempt) + ": " +
                    std::string(svp::exec::attempt_event_kind_name(event.kind)) +
                    (event.detail.empty() ? std::string() : " (" + event.detail + ")");
      stop.request();
      return;
    }
    if (event.kind == svp::exec::AttemptEventKind::leased && timing.first_lease.count() < 0) {
      timing.first_lease = clock.now();
    }
    if (event.kind != svp::exec::AttemptEventKind::committed) {
      return;
    }
    timing.last_commit = clock.now();
    if (++timing.committed != timing.tasks || found->second == 0) {
      return;
    }
    const std::size_t slots = found->second;
    const std::int64_t wall =
        std::max<std::int64_t>(1, (timing.last_commit - timing.first_lease).count());
    const std::uint64_t items = timing.tasks * workload.items_per_batch;
    const double rate = static_cast<double>(items) * 1000.0 / static_cast<double>(wall);
    result.sweep.push_back(
        CapacityStep{.slots = slots, .items = items, .wall_ms = wall, .items_per_second = rate});
    if (slots == 1 || rate >= best * (1.0 + kCapacityMinSlotGain)) {
      best = rate;
      best_slots = slots;
      best_seconds_per_item = static_cast<double>(slots) / rate;
      if (slots == max_slots) {
        result.stopped_because = "reached the most slots this Mac's memory and CPUs admit";
      }
      return;
    }
    result.stopped_because = std::to_string(slots) + " slots added less than " +
                             std::to_string(static_cast<int>(kCapacityMinSlotGain * 100)) +
                             "% throughput over " + std::to_string(best_slots);
    stop.request();
  };

  svp::exec::CancellationToken combined;
  svp::exec::InMemoryResultCommitSink sink;
  std::vector<svp::exec::Executor*> executors{&executor};
  // The scheduler checks one token; forward both the caller's and ours.
  const svp::exec::AttemptObserver forwarding = [&](const svp::exec::AttemptEvent& event) {
    observer(event);
    if (stop.requested() || cancellation.requested()) {
      combined.request();
    }
  };
  const svp::exec::BuildOutcome outcome =
      svp::exec::Scheduler(svp::exec::SchedulerPolicy{}, clock)
          .run(graph, executors, sink, combined, forwarding);
  if (cancellation.requested()) {
    throw std::runtime_error("calibration cancelled");
  }
  if (!interrupted.empty()) {
    throw std::runtime_error("calibration was interrupted, nothing is kept: " + interrupted);
  }
  if (outcome.status == svp::exec::BuildStatus::failed) {
    throw std::runtime_error("calibration batch failed: " +
                             (outcome.failure ? outcome.failure->message : std::string()));
  }
  if (result.sweep.empty()) {
    throw std::runtime_error("calibration measured no step");
  }
  result.slots = best_slots;
  result.seconds_per_item = best_seconds_per_item;
  if (result.stopped_because.empty()) {
    result.stopped_because = "swept every admissible slot count";
  }
  return result;
}

}  // namespace svp::builder::calibration
