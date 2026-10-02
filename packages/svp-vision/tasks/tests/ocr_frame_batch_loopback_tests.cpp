// Real-model determinism check for ocr.frame_batch: the same frame batches,
// run (1) directly in-process the way the local OCR stage runs them, (2)
// through the task engine's InProcessExecutor, and (3) through the
// LoopbackExecutor's child worker process with its own PP-OCR sessions, must
// produce byte-identical payloads, which assemble to the full sample plan.
//
// Needs real inputs, so it skips (prints "Skipping:") unless these are set:
//   SVP_OCR_TASK_TEST_SOURCE         source video
//   SVP_OCR_TASK_TEST_MODEL_CACHE    model cache holding the PP-OCRv6 bundles
//   SVP_OCR_TASK_TEST_FFMPEG         ffmpeg executable
//   SVP_OCR_TASK_TEST_TIMESTAMPS_US  comma-separated sample timestamps
//   SVP_OCR_TASK_TEST_FRAME_SIZE     decode size, "<width>x<height>"
// Optional:
//   SVP_OCR_TASK_TEST_FIXTURE_OUT    write the assembled JSONL payload here
//                                    (how the reducer test fixture is made)
//
// argv[1] is the svp-vision-ocr-task-test-worker executable.

#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/clock.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/loopback_executor.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/models/thread_plan.hpp"
#include "svp/vision/inference_performance.hpp"
#include "svp/vision/ocr_frame_batch.hpp"
#include "svp/vision/ocr_frame_batch_reduction.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"
#include "svp/vision/tasks/ocr_frame_batch_task.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace exec = svp::exec;
namespace vision = svp::vision;
namespace tasks = svp::vision::tasks;

// More than one batch, so results of several tasks are reassembled; the plan
// is split into this many batches of near-equal size.
constexpr std::size_t kTestBatchCount = 3;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::optional<std::string> env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "cannot read " + path.string());
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    bytes[index] = static_cast<std::byte>(text[index]);
  }
  return bytes;
}

std::string to_text(const std::vector<std::byte>& bytes) {
  std::string text(bytes.size(), '\0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    text[index] = static_cast<char>(bytes[index]);
  }
  return text;
}

struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "svp-ocr-loopback-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::vector<std::string> run_graph(const exec::TaskGraph& graph, exec::Executor& executor,
                                   const std::string& label) {
  exec::InMemoryResultCommitSink sink;
  const exec::SteadyClock clock;
  const exec::CancellationToken cancellation;
  std::vector<exec::Executor*> executors = {&executor};
  const exec::BuildOutcome outcome =
      exec::Scheduler(exec::SchedulerPolicy{}, clock).run(graph, executors, sink, cancellation);
  require(outcome.status == exec::BuildStatus::succeeded,
          label + " run failed" +
              (outcome.failure ? ": " + outcome.failure->message : std::string()));
  std::vector<std::string> payloads;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const exec::CommittedResult* committed = sink.find(graph.node(index).spec.task_id);
    require(committed != nullptr && committed->payloads.size() == 1,
            label + " committed no payload for " + graph.node(index).spec.task_id);
    payloads.push_back(to_text(committed->payloads.front()));
  }
  return payloads;
}

int run(const std::filesystem::path& worker) {
  const auto source = env("SVP_OCR_TASK_TEST_SOURCE");
  const auto model_cache = env("SVP_OCR_TASK_TEST_MODEL_CACHE");
  const auto ffmpeg = env("SVP_OCR_TASK_TEST_FFMPEG");
  const auto timestamps_text = env("SVP_OCR_TASK_TEST_TIMESTAMPS_US");
  const auto frame_size = env("SVP_OCR_TASK_TEST_FRAME_SIZE");
  if (!source || !model_cache || !ffmpeg || !timestamps_text || !frame_size) {
    std::cout << "Skipping: set SVP_OCR_TASK_TEST_{SOURCE,MODEL_CACHE,FFMPEG,"
                 "TIMESTAMPS_US,FRAME_SIZE} to run\n";
    return 0;
  }
  const auto timestamps = vision::parse_ocr_diagnostic_timestamps(timestamps_text->c_str());
  require(timestamps.has_value(), "SVP_OCR_TASK_TEST_TIMESTAMPS_US is malformed");
  int width = 0;
  int height = 0;
  require(std::sscanf(frame_size->c_str(), "%dx%d", &width, &height) == 2,
          "SVP_OCR_TASK_TEST_FRAME_SIZE must be <width>x<height>");

  vision::OcrTemporalSamplingResult sampling;
  sampling.timestamps_us = *timestamps;
  sampling.sample_count = static_cast<int>(timestamps->size());
  const vision::OcrSamplePlan plan =
      vision::make_ocr_sample_plan(std::move(sampling), width, height);

  // The stage's own option resolution, with the local build's thread plan.
  const svp::models::ThreadPlan threads = svp::models::resolve_local_thread_plan(
      svp::models::detect_host_cpu_topology(),
      vision::recognition_workers_for_ocr_profile(
          vision::InferencePerformanceOptions{}.ocr_performance_profile));
  vision::OcrGenerationOptions stage;
  stage.model_cache_root = *model_cache;
  stage.recognition_parallel_workers = threads.ocr_recognition_workers;
  stage.detection_threads = threads.ocr_detection;
  stage.recognition_threads = threads.ocr_recognition;
  const svp::vision::PpOcrOptions pp_ocr = vision::make_ocr_pp_ocr_options(stage);

  const std::uint64_t per_batch = static_cast<std::uint64_t>(std::ceil(
      static_cast<double>(plan.samples.size()) / static_cast<double>(kTestBatchCount)));
  const vision::OcrBatchPolicy policy{.target_task_seconds = static_cast<double>(per_batch),
                                      .estimated_seconds_per_sample = 1.0};
  const std::vector<vision::OcrSampleBatch> batches =
      vision::partition_ocr_samples(plan.samples.size(), policy);

  // (1) Direct, as generate_ocr_observations runs batches.
  std::vector<std::string> direct;
  {
    const vision::PpOcrSession session = vision::create_pp_ocr_session(pp_ocr);
    require(session.available, "PP-OCR unavailable: " + session.blocker);
    for (const vision::OcrSampleBatch& batch : batches) {
      const auto first = plan.samples.begin() + static_cast<std::ptrdiff_t>(batch.first_ordinal);
      const vision::OcrFrameBatchOutcome outcome = vision::run_ocr_frame_batch(
          session, pp_ocr,
          vision::OcrFrameBatchRequest{
              .source_path = *source,
              .ffmpeg_path = *ffmpeg,
              .frame_width = width,
              .frame_height = height,
              .samples = std::vector<vision::OcrSample>(
                  first, first + static_cast<std::ptrdiff_t>(batch.count))});
      require(outcome.decoding_attempted, "direct decode: " + outcome.skipped_reason);
      direct.push_back(vision::encode_ocr_sample_detections_jsonl(outcome.samples));
    }
  }

  const TemporaryDirectory scratch;
  const std::filesystem::path cas_root = scratch.path / "cas";
  exec::CacheResult<exec::CasStore> store = exec::CasStore::at(cas_root);
  require(static_cast<bool>(store), "cache unavailable");
  exec::CasTaskArtifactAccess artifacts(std::move(store).value(),
                                        "bs_ocr_loopback_" + std::to_string(::getpid()));
  const std::vector<std::byte> source_bytes = read_file(*source);
  const exec::ArtifactRef source_ref = artifacts.put(
      source_bytes, "video/mp4", std::string(tasks::kOcrFrameBatchSourceRole));

  const tasks::OcrFrameBatchTaskInputs inputs{
      .build_session_id = "bs_ocr_loopback",
      .depends_on = {},
      .source = source_ref,
      .model_refs = tasks::ocr_frame_batch_model_refs(pp_ocr),
      .pp_ocr = pp_ocr,
      .ffmpeg_build = tasks::ffmpeg_build_identity(*ffmpeg).value(),
      .batch_policy = policy,
  };
  std::vector<exec::TaskNode> nodes;
  for (const vision::OcrSampleBatch& batch : batches) {
    nodes.push_back(exec::TaskNode{
        .spec = tasks::make_ocr_frame_batch_task_spec(inputs, plan, batch),
        .order_key = tasks::ocr_frame_batch_order_key(batch)});
  }
  const exec::TaskGraph graph(nodes);

  // (2) The task engine, in this process.
  std::vector<std::string> in_process;
  {
    exec::TaskTypeRegistry registry;
    tasks::register_ocr_frame_batch_task(
        registry, tasks::OcrFrameBatchWorkerEnvironment{
                      .model_cache_root = *model_cache,
                      .ffmpeg_path = *ffmpeg,
                      .write_output =
                          [&artifacts](std::span<const std::byte> bytes,
                                       std::string media_type, std::string role) {
                            return artifacts.put(bytes, std::move(media_type),
                                                 std::move(role));
                          }});
    exec::InProcessExecutor executor(registry, artifacts, {.threads = 1});
    in_process = run_graph(graph, executor, "in-process executor");
  }

  // (3) The loopback executor's child process.
  exec::LoopbackExecutor loopback(exec::LoopbackExecutorOptions{
      .executor_id = "loopback",
      .worker_executable = worker,
      .worker_arguments = {"--cas-root", cas_root.string(), "--model-cache", *model_cache,
                           "--ffmpeg", *ffmpeg},
      .slots = 1});
  const std::vector<std::string> child = run_graph(graph, loopback, "loopback executor");

  std::vector<std::vector<vision::OcrSampleDetections>> batch_records;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const std::string& task_id = graph.node(index).spec.task_id;
    require(direct[index] == in_process[index],
            task_id + ": in-process executor payload differs from the direct run");
    require(direct[index] == child[index],
            task_id + ": loopback payload differs from the direct run");
    batch_records.push_back(
        tasks::read_ocr_frame_batch_output(graph.node(index).spec, child[index]));
  }
  const std::vector<vision::OcrSampleDetections> assembled =
      vision::assemble_ocr_frame_batches(plan, std::move(batch_records));
  const std::string assembled_bytes = vision::encode_ocr_sample_detections_jsonl(assembled);

  std::size_t detections = 0;
  for (const auto& sample : assembled) detections += sample.detections.size();
  std::cout << "ocr.frame_batch: " << graph.size() << " batches, " << assembled.size()
            << " samples, " << detections << " detections, " << assembled_bytes.size()
            << " payload bytes; direct == in-process executor == loopback child\n";

  if (const auto fixture = env("SVP_OCR_TASK_TEST_FIXTURE_OUT")) {
    std::ofstream(*fixture, std::ios::binary) << assembled_bytes;
    std::cout << "wrote " << *fixture << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <svp-vision-ocr-task-test-worker>\n";
    return 2;
  }
  try {
    return run(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << "Test failed: " << error.what() << "\n";
    return 1;
  }
}
