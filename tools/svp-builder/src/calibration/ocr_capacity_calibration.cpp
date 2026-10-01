#include "calibration/ocr_capacity_calibration.hpp"

#include "svp/exec/clock.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/vision/ocr_sample_plan.hpp"
#include "svp/vision/tasks/ocr_calibration_clip.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace svp::builder::calibration {
namespace {

constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;
constexpr std::string_view kCalibrationLane = "calibration";

std::string task_id(std::size_t step, std::size_t chain, std::size_t batch) {
  return "task.calibration.ocr.slots_" + std::to_string(step) + ".chain_" +
         std::to_string(chain) + ".batch_" + std::to_string(batch);
}

svp::vision::OcrSamplePlan clip_plan(const std::vector<std::int64_t>& timestamps_us) {
  svp::vision::OcrTemporalSamplingResult sampling;
  sampling.timestamps_us = timestamps_us;
  sampling.sample_count = static_cast<int>(timestamps_us.size());
  return svp::vision::make_ocr_sample_plan(
      std::move(sampling), svp::vision::tasks::kOcrCalibrationFrameWidth,
      svp::vision::tasks::kOcrCalibrationFrameHeight);
}

struct StepTiming {
  std::size_t tasks = 0;
  std::size_t committed = 0;
  std::chrono::milliseconds first_lease{-1};
  std::chrono::milliseconds last_commit{0};
};

}  // namespace

nlohmann::json ocr_calibration_to_json(const OcrCalibration& calibration) {
  nlohmann::json sweep = nlohmann::json::array();
  for (const OcrCalibrationStep& step : calibration.sweep) {
    sweep.push_back({{"frames", step.frames},
                     {"frames_per_second", step.frames_per_second},
                     {"slots", step.slots},
                     {"wall_ms", step.wall_ms}});
  }
  return {{"seconds_per_frame", calibration.seconds_per_frame},
          {"slots", calibration.slots},
          {"stopped_because", calibration.stopped_because},
          {"sweep", std::move(sweep)}};
}

OcrCalibration ocr_calibration_from_json(const nlohmann::json& value) {
  OcrCalibration calibration;
  calibration.slots = value.at("slots").get<std::size_t>();
  calibration.seconds_per_frame = value.at("seconds_per_frame").get<double>();
  calibration.stopped_because = value.at("stopped_because").get<std::string>();
  for (const nlohmann::json& step : value.at("sweep")) {
    calibration.sweep.push_back(OcrCalibrationStep{
        .slots = step.at("slots").get<std::size_t>(),
        .frames = step.at("frames").get<std::uint64_t>(),
        .wall_ms = step.at("wall_ms").get<std::int64_t>(),
        .frames_per_second = step.at("frames_per_second").get<double>()});
  }
  if (calibration.slots == 0 || !(calibration.seconds_per_frame > 0.0)) {
    throw std::invalid_argument("calibration needs slots >= 1 and a positive frame time");
  }
  return calibration;
}

std::size_t ocr_calibration_max_slots(std::uint64_t available_bytes, std::uint64_t reserve_bytes,
                                      std::uint32_t logical_cpus) {
  const std::uint64_t usable = available_bytes > reserve_bytes ? available_bytes - reserve_bytes : 0;
  const std::uint64_t by_memory =
      usable / (svp::vision::tasks::kOcrFrameBatchEstimatedPeakRssMb * kBytesPerMiB);
  const std::uint64_t by_cpu = std::max<std::uint32_t>(1, logical_cpus);
  return static_cast<std::size_t>(std::max<std::uint64_t>(1, std::min(by_memory, by_cpu)));
}

svp::exec::TaskSpec ocr_calibration_spec(const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
                                         const std::vector<std::int64_t>& timestamps_us) {
  const svp::vision::OcrSamplePlan plan = clip_plan(timestamps_us);
  return svp::vision::tasks::make_ocr_frame_batch_task_spec(
      inputs, plan,
      svp::vision::OcrSampleBatch{.first_ordinal = 0, .count = plan.samples.size()});
}

OcrCalibration calibrate_ocr_capacity(svp::exec::Executor& executor, std::size_t max_slots,
                                      const svp::vision::tasks::OcrFrameBatchTaskInputs& inputs,
                                      const std::vector<std::int64_t>& timestamps_us,
                                      const svp::exec::CancellationToken& cancellation) {
  if (max_slots == 0 || executor.slots() < max_slots) {
    throw std::invalid_argument("calibration executor has fewer slots than the sweep needs");
  }
  const svp::exec::TaskSpec batch = ocr_calibration_spec(inputs, timestamps_us);
  const std::uint64_t frames_per_batch = timestamps_us.size();
  const svp::exec::TaskSpec warm_up = svp::vision::tasks::make_ocr_frame_batch_task_spec(
      inputs, clip_plan(timestamps_us), svp::vision::OcrSampleBatch{.first_ordinal = 0, .count = 1});

  // Step 0 is the warm-up (one task); step k has k chains.
  std::vector<svp::exec::TaskNode> nodes;
  std::map<std::string, std::size_t> step_of;
  std::vector<std::string> previous_step;
  for (std::size_t step = 0; step <= max_slots; ++step) {
    const std::size_t chains = std::max<std::size_t>(1, step);
    const std::size_t per_chain = step == 0 ? 1 : kOcrCalibrationBatchesPerSlot;
    std::vector<std::string> this_step;
    for (std::size_t chain = 0; chain < chains; ++chain) {
      for (std::size_t index = 0; index < per_chain; ++index) {
        svp::exec::TaskSpec spec = step == 0 ? warm_up : batch;
        spec.task_id = task_id(step, chain, index);
        spec.depends_on = index == 0 ? previous_step
                                     : std::vector<std::string>{task_id(step, chain, index - 1)};
        std::sort(spec.depends_on.begin(), spec.depends_on.end());
        step_of[spec.task_id] = step;
        nodes.push_back(svp::exec::TaskNode{
            .spec = std::move(spec),
            .order_key = {.lane = std::string(kCalibrationLane),
                          .ordinals = {step, chain, index}}});
      }
      this_step.push_back(task_id(step, chain, per_chain - 1));
    }
    previous_step = std::move(this_step);
  }
  const svp::exec::TaskGraph graph(std::move(nodes));

  std::vector<StepTiming> steps(max_slots + 1);
  for (const auto& [id, step] : step_of) {
    ++steps[step].tasks;
  }
  OcrCalibration result;
  double best = 0.0;
  std::size_t best_slots = 1;
  double best_seconds_per_frame = 0.0;
  svp::exec::CancellationToken stop;
  const svp::exec::SteadyClock clock;
  const svp::exec::AttemptObserver observer = [&](const svp::exec::AttemptEvent& event) {
    const auto found = step_of.find(event.task_id);
    if (found == step_of.end()) {
      return;
    }
    StepTiming& timing = steps[found->second];
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
    const std::int64_t wall = std::max<std::int64_t>(
        1, (timing.last_commit - timing.first_lease).count());
    const std::uint64_t frames = timing.tasks * frames_per_batch;
    const double rate = static_cast<double>(frames) * 1000.0 / static_cast<double>(wall);
    result.sweep.push_back(OcrCalibrationStep{
        .slots = slots, .frames = frames, .wall_ms = wall, .frames_per_second = rate});
    if (slots == 1 || rate >= best * (1.0 + kOcrCalibrationMinSlotGain)) {
      best = rate;
      best_slots = slots;
      best_seconds_per_frame = static_cast<double>(slots) / rate;
      if (slots == max_slots) {
        result.stopped_because = "reached the most slots this Mac's memory and CPUs admit";
      }
      return;
    }
    result.stopped_because = std::to_string(slots) + " slots added less than " +
                             std::to_string(static_cast<int>(kOcrCalibrationMinSlotGain * 100)) +
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
  if (outcome.status == svp::exec::BuildStatus::failed) {
    throw std::runtime_error("calibration batch failed: " +
                             (outcome.failure ? outcome.failure->message : std::string()));
  }
  if (result.sweep.empty()) {
    throw std::runtime_error("calibration measured no step");
  }
  result.slots = best_slots;
  result.seconds_per_frame = best_seconds_per_frame;
  if (result.stopped_because.empty()) {
    result.stopped_because = "swept every admissible slot count";
  }
  return result;
}

}  // namespace svp::builder::calibration
