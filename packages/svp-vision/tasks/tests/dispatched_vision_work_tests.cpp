// Real-model checks of the dispatched vision stage work (M4,
// svp/vision/dispatched_work.hpp), on the OCR calibration clip:
//
//   1. Each task type computes, per item, exactly what the stage's per-item
//      function computes: run directly in this process, through the task
//      engine's InProcessExecutor, and through the LoopbackExecutor's child
//      worker process (its own model sessions), the outcomes are identical.
//   2. Each stage given a dispatcher writes byte-identical staging files to
//      the stage without one, whatever the dispatcher returns: outcomes from
//      the tasks, every item reported failed (the stage redoes them), or
//      nullopt (the stage does the work itself). A dispatcher that throws
//      DispatchedWorkError ends the stage instead of becoming a blocker.
//
// Needs the model cache (SVP_MODEL_CACHE_ROOT, with the PP-OCRv6, Nomic
// Embed text and vision, and Depth Anything V2 bundles) and ffmpeg
// ($SVP_FFMPEG or ffmpeg on PATH); prints "Skipping:" otherwise.
//
// argv[1] is the svp-vision-ocr-task-test-worker executable. With
// SVP_DISPATCH_TEST_PEAK_RSS set, each type's loopback child runs alone and
// its peak RSS is printed (how the types' admission estimates were measured).

#include "embedding_generation/shot_keyframe_embedding.hpp"
#include "ocr_generation/ocr_generation_internal.hpp"

#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/clock.hpp"
#include "svp/exec/in_process_executor.hpp"
#include "svp/exec/loopback_executor.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/scheduler.hpp"
#include "svp/media/canonical_raster.hpp"
#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/depth_generation.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/embedding_generation.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/tasks/depth_frame_batch_spec.hpp"
#include "svp/vision/tasks/dispatched_vision_tasks.hpp"
#include "svp/vision/tasks/embed_keyframe_batch_spec.hpp"
#include "svp/vision/tasks/embed_text_batch_spec.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/model_refs.hpp"
#include "svp/vision/tasks/ocr_calibration_clip.hpp"
#include "svp/vision/tasks/ocr_crop_batch_spec.hpp"
#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"
#include "svp/vision/tasks/ocr_frame_batch_spec.hpp"
#include "svp/vision/text_tokenizer.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace exec = svp::exec;
namespace vision = svp::vision;
namespace tasks = svp::vision::tasks;
namespace fs = std::filesystem;
using Progress = std::function<void(std::size_t, std::size_t)>;

// Items are cut into batches of this many, so every type's results come
// back from several tasks and are reassembled in order.
constexpr double kTestItemsPerBatch = 7.0;
// Explicit thread counts, as a build's thread plan sends them.
constexpr svp::models::OrtThreadCounts kTestThreads{.intra_op = 2, .inter_op = 1};
constexpr std::uint32_t kEmbeddingDim = 768;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::optional<std::string> env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
}

struct TemporaryDirectory {
  fs::path path;
  explicit TemporaryDirectory(const std::string& stem) {
    std::string pattern = (fs::temp_directory_path() / (stem + "-XXXXXX")).string();
    require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

std::vector<std::byte> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "cannot read " + path.string());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

// Every regular file under `root`, by relative path, with its bytes.
std::map<std::string, std::vector<std::byte>> snapshot(const fs::path& root) {
  std::map<std::string, std::vector<std::byte>> files;
  if (!fs::exists(root)) return files;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) {
      files[fs::relative(entry.path(), root).string()] = read_file(entry.path());
    }
  }
  return files;
}

tasks::ItemBatchPolicy test_policy() {
  tasks::ItemBatchPolicy policy;
  policy.estimated_seconds_per_item = policy.target_task_seconds / kTestItemsPerBatch;
  return policy;
}

struct Fixture {
  fs::path model_cache;
  fs::path ffmpeg;
  fs::path worker;
  std::string ffmpeg_build;
  TemporaryDirectory scratch{"svp-dispatched-work"};
  fs::path clip;
  fs::path cas_root;
  std::unique_ptr<exec::CasTaskArtifactAccess> artifacts;
  exec::ArtifactRef source;
  vision::PpOcrOptions pp_ocr;
  svp::media::CanonicalAnalysisRaster raster;
};

exec::TaskTypeRegistry& coordinator_registry(Fixture& fixture) {
  static std::unique_ptr<exec::TaskTypeRegistry> registry;
  if (!registry) {
    registry = std::make_unique<exec::TaskTypeRegistry>();
    tasks::register_dispatched_vision_tasks(
        *registry, tasks::DispatchedTaskEnvironment{
                       .model_cache_root = fixture.model_cache,
                       .model_cache_for = {},
                       .ffmpeg_path = fixture.ffmpeg,
                       .scratch_dir = fixture.scratch.path,
                       .write_output =
                           [&fixture](std::span<const std::byte> bytes, std::string media_type,
                                      std::string role) {
                             return fixture.artifacts->put(bytes, std::move(media_type),
                                                           std::move(role));
                           },
                       .record_start_failures = true});
  }
  return *registry;
}

// Runs `nodes` on `executor` and returns the committed results in node order.
std::vector<exec::CommittedResult> run_nodes(const std::vector<exec::TaskNode>& nodes,
                                             exec::Executor& executor,
                                             const std::string& label) {
  exec::InMemoryResultCommitSink sink;
  const exec::SteadyClock clock;
  const exec::CancellationToken cancellation;
  std::vector<exec::Executor*> executors = {&executor};
  const exec::BuildOutcome outcome = exec::Scheduler(exec::SchedulerPolicy{}, clock)
                                         .run(exec::TaskGraph(nodes), executors, sink,
                                              cancellation);
  require(outcome.status == exec::BuildStatus::succeeded,
          label + " failed" + (outcome.failure ? ": " + outcome.failure->message : ""));
  std::vector<exec::CommittedResult> results;
  for (const exec::TaskNode& node : nodes) {
    results.push_back(*sink.find(node.spec.task_id));
  }
  return results;
}

std::unique_ptr<exec::LoopbackExecutor> loopback(Fixture& fixture) {
  return std::make_unique<exec::LoopbackExecutor>(exec::LoopbackExecutorOptions{
      .executor_id = "loopback",
      .worker_executable = fixture.worker,
      .worker_arguments = {"--cas-root", fixture.cas_root.string(), "--model-cache",
                           fixture.model_cache.string(), "--ffmpeg", fixture.ffmpeg.string(),
                           "--scratch", fixture.scratch.path.string()},
      .slots = 1});
}

std::uint64_t children_peak_rss_mib() {
  rusage usage{};
  ::getrusage(RUSAGE_CHILDREN, &usage);
  // macOS reports ru_maxrss in bytes.
  return static_cast<std::uint64_t>(usage.ru_maxrss) / (1024 * 1024);
}

// Runs the type's specs in-process and in the loopback child, checks both
// give the same payload bytes, and returns the in-process outcomes in order.
template <typename Outcome, typename Read>
std::vector<Outcome> run_both(Fixture& fixture, const std::vector<exec::TaskNode>& nodes,
                              const std::string& type, Read read) {
  exec::InProcessExecutor in_process(coordinator_registry(fixture), *fixture.artifacts,
                                     {.executor_id = "in-process", .threads = 1});
  const std::vector<exec::CommittedResult> local = run_nodes(nodes, in_process, type + " in-process");
  std::vector<exec::CommittedResult> child;
  {
    const std::unique_ptr<exec::LoopbackExecutor> executor = loopback(fixture);
    child = run_nodes(nodes, *executor, type + " loopback");
  }
  if (env("SVP_DISPATCH_TEST_PEAK_RSS")) {
    std::cout << type << " loopback worker peak RSS: " << children_peak_rss_mib() << " MiB\n";
  }
  std::vector<Outcome> outcomes;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    require(local[index].payloads == child[index].payloads,
            nodes[index].spec.task_id + ": loopback payload differs from in-process");
    std::vector<Outcome> part =
        read(nodes[index].spec, local[index].result.outputs, local[index].payloads);
    std::move(part.begin(), part.end(), std::back_inserter(outcomes));
  }
  return outcomes;
}

template <typename Make>
std::vector<exec::TaskNode> make_nodes(std::size_t count, Make make) {
  std::vector<exec::TaskNode> nodes;
  for (const tasks::ItemBatch& batch : tasks::partition_items(count, test_policy(), {})) {
    nodes.push_back(make(batch));
  }
  require(nodes.size() > 1, "the test needs several tasks");
  return nodes;
}

svp::models::OnnxSession load_session(const Fixture& fixture, const std::string& model_id,
                                      vision::WordPieceTokenizer* tokenizer = nullptr) {
  const fs::path bundle = fixture.model_cache / model_id;
  const auto manifest = svp::models::load_model_bundle_manifest(bundle / "model.svpmodel.json");
  if (tokenizer != nullptr) {
    for (const auto& file : manifest.files) {
      if (file.role == "tokenizer_vocab") {
        require(tokenizer->load(bundle / file.path), "tokenizer");
      }
    }
  }
  svp::models::OnnxSessionOptions options;
  options.execution_provider = "cpu";
  options.threads = kTestThreads;
  return svp::models::OnnxSession::load(manifest, bundle, options);
}

vision::DispatchedModel dispatched_model(const std::string& model_id) {
  return {.model_id = model_id, .execution_provider = "cpu", .threads = kTestThreads};
}

tasks::OnnxModelParameters onnx_parameters(const std::string& model_id) {
  return {.model_id = model_id, .execution_provider = "cpu", .threads = kTestThreads};
}

// --- evidence crops ----------------------------------------------------------

vision::EvidenceCropOptions crop_options(const Fixture& fixture) {
  vision::EvidenceCropOptions options;
  options.ffmpeg_path = fixture.ffmpeg;
  options.source_media_path = fixture.clip;
  options.ocr_frame_width = tasks::kOcrCalibrationFrameWidth;
  options.ocr_frame_height = tasks::kOcrCalibrationFrameHeight;
  options.source_frame_width = tasks::kOcrCalibrationFrameWidth;
  options.source_frame_height = tasks::kOcrCalibrationFrameHeight;
  options.crop_image_format = vision::kEvidenceCropImageFormat;
  options.jpeg_quality = vision::kEvidenceCropJpegQuality;
  return options;
}

// One observation per text line of the clip's two lightest frames, plus one
// whose box lies outside the frame (it gets no crop rectangle).
std::vector<vision::CropGenerationInput> crop_inputs() {
  const std::vector<std::int64_t> timestamps = tasks::ocr_calibration_timestamps_us();
  std::vector<vision::CropGenerationInput> inputs;
  for (const auto& line : tasks::ocr_calibration_text_lines()) {
    if (line.frame > 1) continue;
    vision::CropGenerationInput input;
    input.text_region_id = "text_region_" + std::to_string(inputs.size() + 1);
    input.text_observation_id = "text_obs_" + std::to_string(inputs.size() + 1);
    input.source_frame_id = "frame_" + std::to_string(line.frame + 1);
    input.source_timestamp_us = timestamps[line.frame];
    input.bbox_left = line.left;
    input.bbox_top = line.top;
    input.bbox_right = line.right;
    input.bbox_bottom = line.bottom;
    input.frame_width = tasks::kOcrCalibrationFrameWidth;
    input.frame_height = tasks::kOcrCalibrationFrameHeight;
    input.confidence = 0.9;
    input.observation_raw_text = line.text;
    inputs.push_back(std::move(input));
  }
  vision::CropGenerationInput outside = inputs.front();
  outside.text_observation_id = "text_obs_outside";
  outside.bbox_left = outside.bbox_right = tasks::kOcrCalibrationFrameWidth + 100;
  outside.bbox_top = outside.bbox_bottom = tasks::kOcrCalibrationFrameHeight + 100;
  inputs.insert(inputs.begin() + 3, outside);
  return inputs;
}

std::vector<exec::TaskNode> crop_nodes(Fixture& fixture,
                                       const std::vector<vision::EvidenceCropJob>& jobs) {
  const tasks::OcrCropBatchTaskInputs inputs{
      .build_session_id = "bs_dispatched_test",
      .depends_on = {},
      .source = fixture.source,
      .model_refs = tasks::ocr_frame_batch_model_refs(fixture.pp_ocr),
      .pp_ocr = fixture.pp_ocr,
      .ffmpeg_build = fixture.ffmpeg_build,
      .batch_policy = test_policy()};
  return make_nodes(jobs.size(), [&](const tasks::ItemBatch& batch) {
    return exec::TaskNode{.spec = tasks::make_ocr_crop_batch_task_spec(inputs, jobs, batch),
                          .order_key = tasks::ocr_crop_batch_order_key(batch)};
  });
}

void test_crop_tasks(Fixture& fixture) {
  const std::vector<vision::EvidenceCropJob> jobs =
      vision::plan_evidence_crop_jobs(crop_options(fixture), crop_inputs());
  const vision::PpOcrSession session = vision::create_pp_ocr_session(fixture.pp_ocr);
  require(session.available, "PP-OCR unavailable: " + session.blocker);
  std::vector<vision::EvidenceCropJobOutcome> direct;
  for (const vision::EvidenceCropJob& job : jobs) {
    direct.push_back(vision::run_evidence_crop_job(fixture.ffmpeg, fixture.clip, session,
                                                   fixture.pp_ocr, job,
                                                   fixture.scratch.path / "direct.jpg"));
  }
  const auto tasked = run_both<vision::EvidenceCropJobOutcome>(
      fixture, crop_nodes(fixture, jobs), "ocr.crop_batch", tasks::read_ocr_crop_batch_output);
  require(tasked == direct, "crop tasks differ from the stage's per-observation work");
  std::size_t read_text = 0;
  for (const auto& outcome : direct) read_text += outcome.roi.text.empty() ? 0 : 1;
  require(read_text > 0, "the ROI re-read reads the clip's text");
  std::cout << "ocr.crop_batch: " << jobs.size() << " crops, " << read_text
            << " re-read with text; direct == in-process == loopback\n";
}

// The OCR stage's evidence-crop step on synthetic observations, with
// `dispatcher` (or none), into a fresh staging directory.
std::map<std::string, std::vector<std::byte>> run_crop_stage(
    Fixture& fixture, const vision::PpOcrSession& session,
    vision::EvidenceCropDispatcher dispatcher, std::string* observations_out = nullptr) {
  namespace internal = vision::ocr_generation_internal;
  const TemporaryDirectory staging("svp-dispatched-crop-stage");
  svp::media::MediaIngestPlan plan;
  plan.source_path = fixture.clip;
  plan.primary_video_stream.width = tasks::kOcrCalibrationFrameWidth;
  plan.primary_video_stream.height = tasks::kOcrCalibrationFrameHeight;
  vision::OcrGenerationOptions options;
  options.ffmpeg_path = fixture.ffmpeg;
  options.media_plan = &plan;
  options.ocr_frame_width = tasks::kOcrCalibrationFrameWidth;
  options.ocr_frame_height = tasks::kOcrCalibrationFrameHeight;
  options.canonical_raster_width = fixture.raster.width;
  options.canonical_raster_height = fixture.raster.height;
  options.generate_evidence_crops = true;
  options.evidence_crop_dispatcher = std::move(dispatcher);

  vision::OcrGenerationResult result;
  std::vector<internal::ReconciledObservation> reconciled;
  for (const vision::CropGenerationInput& input : crop_inputs()) {
    vision::TextObservationRecord observation;
    observation.text_observation_id = input.text_observation_id;
    observation.text_region_id = input.text_region_id;
    observation.source_frame_ids = {input.source_frame_id};
    // A lower-case reading the ROI re-read may improve.
    observation.raw_text = input.observation_raw_text;
    for (char& character : observation.raw_text) character = static_cast<char>(std::tolower(character));
    observation.normalized_text = observation.raw_text;
    observation.confidence = input.confidence;
    result.text_observations.push_back(observation);
    internal::ReconciledObservation robs;
    robs.start_us = input.source_timestamp_us;
    robs.bbox_left = input.bbox_left;
    robs.bbox_top = input.bbox_top;
    robs.bbox_right = input.bbox_right;
    robs.bbox_bottom = input.bbox_bottom;
    robs.frame_width = input.frame_width;
    robs.frame_height = input.frame_height;
    robs.confidence = input.confidence;
    reconciled.push_back(robs);
  }
  fs::create_directories(staging.path / "text");
  (void)internal::generate_and_harden_evidence_crops(options, reconciled, session, fixture.pp_ocr,
                                                     staging.path, result);
  if (observations_out != nullptr) {
    observations_out->clear();
    for (const auto& observation : result.text_observations) {
      *observations_out += observation.raw_text + "|" + std::to_string(observation.confidence) +
                           "|" + std::to_string(observation.evidence_crop_refs.size()) + "\n";
    }
  }
  return snapshot(staging.path);
}

void test_crop_stage(Fixture& fixture) {
  const vision::PpOcrSession session = vision::create_pp_ocr_session(fixture.pp_ocr);
  std::string local_observations;
  const auto local = run_crop_stage(fixture, session, {}, &local_observations);
  require(local.size() > 2, "the crop stage writes crops");

  const vision::EvidenceCropDispatcher tasks_dispatcher =
      [&](const std::vector<vision::EvidenceCropJob>& jobs, const vision::PpOcrOptions& roi,
          const Progress&) -> std::optional<std::vector<vision::EvidenceCropJobOutcome>> {
    require(roi.detector_model_id == fixture.pp_ocr.detector_model_id, "ROI options passed");
    exec::InProcessExecutor executor(coordinator_registry(fixture), *fixture.artifacts,
                                     {.executor_id = "in-process", .threads = 2});
    const std::vector<exec::TaskNode> nodes = crop_nodes(fixture, jobs);
    const auto results = run_nodes(nodes, executor, "crop dispatcher");
    std::vector<vision::EvidenceCropJobOutcome> outcomes;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      auto part = tasks::read_ocr_crop_batch_output(nodes[index].spec, results[index].result.outputs,
                                                    results[index].payloads);
      std::move(part.begin(), part.end(), std::back_inserter(outcomes));
    }
    return outcomes;
  };
  std::string dispatched_observations;
  require(run_crop_stage(fixture, session, tasks_dispatcher, &dispatched_observations) == local &&
              dispatched_observations == local_observations,
          "crops from tasks write the same staging and observations as the stage alone");

  const vision::EvidenceCropDispatcher all_failed =
      [](const std::vector<vision::EvidenceCropJob>& jobs, const vision::PpOcrOptions&,
         const Progress&) {
        std::vector<vision::EvidenceCropJobOutcome> outcomes;
        for (const auto& job : jobs) outcomes.push_back({.ordinal = job.ordinal});
        return std::optional(outcomes);
      };
  std::string failed_observations;
  require(run_crop_stage(fixture, session, all_failed, &failed_observations) == local &&
              failed_observations == local_observations,
          "crops the dispatcher could not make are made by the stage itself");

  const vision::EvidenceCropDispatcher hand_back =
      [](const std::vector<vision::EvidenceCropJob>&, const vision::PpOcrOptions&,
         const Progress&) { return std::optional<std::vector<vision::EvidenceCropJobOutcome>>(); };
  require(run_crop_stage(fixture, session, hand_back) == local,
          "a dispatcher that hands the work back changes nothing");

  bool threw = false;
  try {
    (void)run_crop_stage(fixture, session,
                         [](const auto&, const auto&, const Progress&)
                             -> std::optional<std::vector<vision::EvidenceCropJobOutcome>> {
                           throw vision::DispatchedWorkError("workers gone");
                         });
  } catch (const vision::DispatchedWorkError&) {
    threw = true;
  }
  require(threw, "a failed dispatch ends the stage instead of becoming a crop blocker");
  std::cout << "evidence crop stage: dispatched == failed-and-redone == handed back == local\n";
}

// --- text and keyframe embeddings ------------------------------------------

std::vector<vision::TextEmbeddingItem> text_items() {
  std::vector<vision::TextEmbeddingItem> items;
  for (const auto& line : tasks::ocr_calibration_text_lines()) {
    if (items.size() == 23) break;
    items.push_back({.id = "text_obs_" + std::to_string(items.size() + 1), .text = line.text});
  }
  return items;
}

std::vector<vision::KeyframeEmbeddingItem> keyframe_items(const Fixture& fixture) {
  std::vector<vision::KeyframeEmbeddingItem> items;
  for (int repeat = 0; repeat < 3; ++repeat) {
    for (const std::int64_t timestamp : tasks::ocr_calibration_timestamps_us()) {
      items.push_back({.shot_id = "shot_" + std::to_string(items.size() + 1),
                       .pts_us = timestamp,
                       .width = fixture.raster.width,
                       .height = fixture.raster.height});
    }
  }
  return items;
}

std::vector<exec::TaskNode> text_nodes(Fixture& fixture,
                                       const std::vector<vision::TextEmbeddingItem>& items) {
  const tasks::EmbedTextBatchTaskInputs inputs{
      .build_session_id = "bs_dispatched_test",
      .depends_on = {},
      .model_ref = tasks::cached_model_ref(fixture.model_cache, svp::models::kNomicEmbedTextV15ModelId),
      .model = onnx_parameters(svp::models::kNomicEmbedTextV15ModelId),
      .embedding_dim = kEmbeddingDim,
      .batch_policy = test_policy()};
  return make_nodes(items.size(), [&](const tasks::ItemBatch& batch) {
    return exec::TaskNode{.spec = tasks::make_embed_text_batch_task_spec(inputs, items, batch),
                          .order_key = tasks::embed_text_batch_order_key(batch)};
  });
}

std::vector<exec::TaskNode> keyframe_nodes(Fixture& fixture,
                                           const std::vector<vision::KeyframeEmbeddingItem>& items) {
  const tasks::EmbedKeyframeBatchTaskInputs inputs{
      .build_session_id = "bs_dispatched_test",
      .depends_on = {},
      .source = fixture.source,
      .model_ref =
          tasks::cached_model_ref(fixture.model_cache, svp::models::kNomicEmbedVisionV15ModelId),
      .model = onnx_parameters(svp::models::kNomicEmbedVisionV15ModelId),
      .embedding_dim = kEmbeddingDim,
      .ffmpeg_build = fixture.ffmpeg_build,
      .batch_policy = test_policy()};
  return make_nodes(items.size(), [&](const tasks::ItemBatch& batch) {
    return exec::TaskNode{.spec = tasks::make_embed_keyframe_batch_task_spec(inputs, items, batch),
                          .order_key = tasks::embed_keyframe_batch_order_key(batch)};
  });
}

void test_text_tasks(Fixture& fixture) {
  vision::WordPieceTokenizer tokenizer;
  const svp::models::OnnxSession text_session =
      load_session(fixture, svp::models::kNomicEmbedTextV15ModelId, &tokenizer);
  std::vector<vision::TextEmbeddingOutcome> direct_text;
  for (const auto& item : text_items()) {
    direct_text.push_back(vision::embed_text_item(text_session, tokenizer, item, kEmbeddingDim));
  }
  require(run_both<vision::TextEmbeddingOutcome>(fixture, text_nodes(fixture, text_items()),
                                                 "embed.text_batch",
                                                 tasks::read_embed_text_batch_output) ==
              direct_text,
          "text tasks differ from the stage's per-observation work");
  std::cout << "embed.text_batch: direct == in-process == loopback\n";
}

void test_keyframe_tasks(Fixture& fixture) {
  const svp::models::OnnxSession vision_session =
      load_session(fixture, svp::models::kNomicEmbedVisionV15ModelId);
  std::vector<vision::KeyframeEmbeddingOutcome> direct_keyframes;
  for (const auto& item : keyframe_items(fixture)) {
    direct_keyframes.push_back(
        vision::embed_keyframe(vision_session, fixture.ffmpeg, fixture.clip, item, kEmbeddingDim));
  }
  require(direct_keyframes.front().embedded, "keyframes embed");
  require(run_both<vision::KeyframeEmbeddingOutcome>(
              fixture, keyframe_nodes(fixture, keyframe_items(fixture)), "embed.keyframe_batch",
              tasks::read_embed_keyframe_batch_output) == direct_keyframes,
          "keyframe tasks differ from the stage's per-keyframe work");
  std::cout << "embed.keyframe_batch: direct == in-process == loopback\n";
}

std::map<std::string, std::vector<std::byte>> run_embedding_stage(
    Fixture& fixture, vision::TextEmbeddingDispatcher text,
    vision::KeyframeEmbeddingDispatcher keyframes) {
  const TemporaryDirectory staging("svp-dispatched-embedding-stage");
  fs::create_directories(staging.path / "text");
  fs::create_directories(staging.path / "timeline");
  {
    std::ofstream out(staging.path / "text" / "text_observations.jsonl");
    for (const auto& item : text_items()) {
      out << nlohmann::json{{"text_observation_id", item.id},
                            {"text_region_id", "region_" + item.id},
                            {"raw_text", item.text},
                            {"normalized_text", item.text}}
                 .dump()
          << "\n";
    }
  }
  {
    std::ofstream frames(staging.path / "timeline" / "frames.jsonl");
    std::ofstream shots(staging.path / "timeline" / "shots.jsonl");
    std::size_t index = 0;
    for (const auto& item : keyframe_items(fixture)) {
      ++index;
      frames << nlohmann::json{{"id", "frame_" + std::to_string(index)},
                               {"pts_us", item.pts_us},
                               {"analysis_width", item.width},
                               {"analysis_height", item.height}}
                    .dump()
             << "\n";
      shots << nlohmann::json{{"id", item.shot_id}, {"start_frame_id", "frame_" + std::to_string(index)}}
                   .dump()
            << "\n";
    }
  }
  svp::media::MediaIngestPlan plan;
  plan.source_path = fixture.clip;
  vision::EmbeddingGenerationOptions options;
  options.model_cache_root = fixture.model_cache;
  options.threads = kTestThreads;
  options.vision_threads = kTestThreads;
  options.media_plan = &plan;
  options.ffmpeg_path = fixture.ffmpeg;
  options.text_dispatcher = std::move(text);
  options.keyframe_dispatcher = std::move(keyframes);
  const vision::EmbeddingGenerationResult result =
      vision::generate_embedding_blocks(options, staging.path);
  require(result.embeddings_blocks_written,
          "the embedding stage writes blocks (" + result.blocker + ")");
  auto files = snapshot(staging.path / "embeddings");
  const std::string provenance = result.processor_provenance.dump();
  files["processor"] = std::vector<std::byte>(reinterpret_cast<const std::byte*>(provenance.data()),
                                              reinterpret_cast<const std::byte*>(provenance.data()) +
                                                  provenance.size());
  return files;
}

void test_embedding_stage(Fixture& fixture) {
  const auto local = run_embedding_stage(fixture, {}, {});
  const vision::TextEmbeddingDispatcher text_tasks =
      [&](const std::vector<vision::TextEmbeddingItem>& items, const vision::DispatchedModel& model,
          std::uint32_t dim, const Progress&) -> std::optional<std::vector<vision::TextEmbeddingOutcome>> {
    require(model == dispatched_model(svp::models::kNomicEmbedTextV15ModelId) && dim == kEmbeddingDim,
            "the stage passes its text model settings");
    exec::InProcessExecutor executor(coordinator_registry(fixture), *fixture.artifacts,
                                     {.executor_id = "in-process", .threads = 2});
    const auto nodes = text_nodes(fixture, items);
    const auto results = run_nodes(nodes, executor, "text dispatcher");
    std::vector<vision::TextEmbeddingOutcome> outcomes;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      auto part = tasks::read_embed_text_batch_output(nodes[index].spec, results[index].result.outputs,
                                                      results[index].payloads);
      std::move(part.begin(), part.end(), std::back_inserter(outcomes));
    }
    return outcomes;
  };
  const vision::KeyframeEmbeddingDispatcher keyframe_tasks =
      [&](const std::vector<vision::KeyframeEmbeddingItem>& items, const vision::DispatchedModel& model,
          std::uint32_t, const Progress&) -> std::optional<std::vector<vision::KeyframeEmbeddingOutcome>> {
    require(model == dispatched_model(svp::models::kNomicEmbedVisionV15ModelId),
            "the stage passes its vision model settings");
    exec::InProcessExecutor executor(coordinator_registry(fixture), *fixture.artifacts,
                                     {.executor_id = "in-process", .threads = 2});
    const auto nodes = keyframe_nodes(fixture, items);
    const auto results = run_nodes(nodes, executor, "keyframe dispatcher");
    std::vector<vision::KeyframeEmbeddingOutcome> outcomes;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      auto part = tasks::read_embed_keyframe_batch_output(
          nodes[index].spec, results[index].result.outputs, results[index].payloads);
      std::move(part.begin(), part.end(), std::back_inserter(outcomes));
    }
    return outcomes;
  };
  require(run_embedding_stage(fixture, text_tasks, keyframe_tasks) == local,
          "embeddings from tasks write the same staging as the stage alone");

  // Every other item failed: the stage redoes those itself.
  const vision::TextEmbeddingDispatcher text_half_failed =
      [&](const std::vector<vision::TextEmbeddingItem>& items, const vision::DispatchedModel& model,
          std::uint32_t dim, const Progress& progress) {
        auto outcomes = text_tasks(items, model, dim, progress);
        for (std::size_t index = 0; index < outcomes->size(); index += 2) {
          (*outcomes)[index] = {.vector = {}, .error = "worker failure"};
        }
        return outcomes;
      };
  const vision::KeyframeEmbeddingDispatcher keyframes_half_failed =
      [&](const std::vector<vision::KeyframeEmbeddingItem>& items,
          const vision::DispatchedModel& model, std::uint32_t dim, const Progress& progress) {
        auto outcomes = keyframe_tasks(items, model, dim, progress);
        for (std::size_t index = 1; index < outcomes->size(); index += 2) {
          (*outcomes)[index] = {};
        }
        return outcomes;
      };
  require(run_embedding_stage(fixture, text_half_failed, keyframes_half_failed) == local,
          "items the dispatcher could not embed are embedded by the stage itself");
  require(run_embedding_stage(
              fixture,
              [](const auto&, const auto&, std::uint32_t, const Progress&) {
                return std::optional<std::vector<vision::TextEmbeddingOutcome>>();
              },
              [](const auto&, const auto&, std::uint32_t, const Progress&) {
                return std::optional<std::vector<vision::KeyframeEmbeddingOutcome>>();
              }) == local,
          "dispatchers that hand the work back change nothing");
  bool threw = false;
  try {
    (void)run_embedding_stage(
        fixture,
        [](const auto& items, const auto&, std::uint32_t, const Progress&) {
          // One outcome short: the stage cannot use it.
          return std::optional(std::vector<vision::TextEmbeddingOutcome>(items.size() - 1));
        },
        {});
  } catch (const vision::DispatchedWorkError&) {
    threw = true;
  }
  require(threw, "outcomes that do not match the items end the stage");
  std::cout << "embedding stage: dispatched == half-failed-and-redone == handed back == local\n";
}

// --- depth ------------------------------------------------------------------

vision::DecodedCanonicalFrames canonical_frames(const Fixture& fixture) {
  svp::media::MediaIngestPlan plan;
  plan.source_path = fixture.clip;
  return vision::decode_frames_at_timestamps(plan, fixture.ffmpeg, fixture.raster.width,
                                             fixture.raster.height,
                                             tasks::ocr_calibration_timestamps_us());
}

std::vector<exec::TaskNode> depth_nodes(Fixture& fixture,
                                        const std::vector<vision::ColorRasterFrame>& frames) {
  std::vector<tasks::DepthFrameItem> items;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    items.push_back(tasks::depth_frame_item(frames[index], index));
  }
  const tasks::DepthFrameBatchTaskInputs inputs{
      .build_session_id = "bs_dispatched_test",
      .depends_on = {},
      .source = fixture.source,
      .model_ref =
          tasks::cached_model_ref(fixture.model_cache, svp::models::kDepthAnythingV2SmallModelId),
      .model = onnx_parameters(svp::models::kDepthAnythingV2SmallModelId),
      .ffmpeg_build = fixture.ffmpeg_build,
      .batch_policy = [] {
        tasks::ItemBatchPolicy policy;
        policy.estimated_seconds_per_item = policy.target_task_seconds / 2;  // 2 per task
        return policy;
      }()};
  std::vector<exec::TaskNode> nodes;
  tasks::ItemBatchPolicy policy = inputs.batch_policy;
  for (const tasks::ItemBatch& batch : tasks::partition_items(items.size(), policy, {})) {
    nodes.push_back({.spec = tasks::make_depth_frame_batch_task_spec(inputs, items, batch),
                     .order_key = tasks::depth_frame_batch_order_key(batch)});
  }
  return nodes;
}

void test_depth(Fixture& fixture) {
  const vision::DecodedCanonicalFrames frames = canonical_frames(fixture);
  require(frames.frames.size() == tasks::ocr_calibration_timestamps_us().size(), "frames decode");
  const svp::models::OnnxSession session =
      load_session(fixture, svp::models::kDepthAnythingV2SmallModelId);
  std::vector<vision::DepthFrameOutcome> direct;
  for (const auto& frame : frames.frames) {
    direct.push_back(vision::infer_depth_frame_outcome(session, frame));
  }
  require(run_both<vision::DepthFrameOutcome>(fixture, depth_nodes(fixture, frames.frames),
                                              "depth.frame_batch",
                                              tasks::read_depth_frame_batch_output) == direct,
          "depth tasks differ from the stage's per-frame work");

  const auto run_stage = [&](vision::DepthFrameDispatcher dispatcher) {
    const TemporaryDirectory staging("svp-dispatched-depth-stage");
    vision::DepthGenerationOptions options;
    options.model_cache_root = fixture.model_cache;
    options.threads = kTestThreads;
    options.raster_width = static_cast<std::uint32_t>(fixture.raster.width);
    options.raster_height = static_cast<std::uint32_t>(fixture.raster.height);
    options.frame_input = frames;
    options.frame_dispatcher = std::move(dispatcher);
    const vision::DepthGenerationResult result = vision::generate_depth_blocks(options, staging.path);
    require(result.depth_blocks_written, "depth writes blocks (" + result.blocker + ")");
    return snapshot(staging.path);
  };
  const auto local = run_stage({});
  const vision::DepthFrameDispatcher via_tasks =
      [&](const std::vector<vision::ColorRasterFrame>& stage_frames, const vision::DispatchedModel& model,
          const Progress&) -> std::optional<std::vector<vision::DepthFrameOutcome>> {
    require(model == dispatched_model(svp::models::kDepthAnythingV2SmallModelId),
            "the stage passes its depth model settings");
    exec::InProcessExecutor executor(coordinator_registry(fixture), *fixture.artifacts,
                                     {.executor_id = "in-process", .threads = 2});
    const auto nodes = depth_nodes(fixture, stage_frames);
    const auto results = run_nodes(nodes, executor, "depth dispatcher");
    std::vector<vision::DepthFrameOutcome> outcomes;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      auto part = tasks::read_depth_frame_batch_output(nodes[index].spec, results[index].result.outputs,
                                                       results[index].payloads);
      std::move(part.begin(), part.end(), std::back_inserter(outcomes));
    }
    return outcomes;
  };
  require(run_stage(via_tasks) == local, "depth from tasks writes the same staging");
  require(run_stage([&](const auto& stage_frames, const auto& model, const Progress& progress) {
            auto outcomes = via_tasks(stage_frames, model, progress);
            (*outcomes)[1] = {.status = vision::DepthFrameStatus::inference_failed};
            return outcomes;
          }) == local,
          "a frame the dispatcher could not run is run by the stage itself");
  std::cout << "depth.frame_batch: direct == in-process == loopback; depth stage dispatched == local\n";
}

int run(const fs::path& worker) {
  const auto model_cache = env("SVP_MODEL_CACHE_ROOT");
  const fs::path ffmpeg = env("SVP_FFMPEG").value_or("ffmpeg");
  if (!model_cache || !vision::ffmpeg_executable_available(ffmpeg)) {
    std::cout << "Skipping: set SVP_MODEL_CACHE_ROOT and have ffmpeg ($SVP_FFMPEG or PATH)\n";
    return 0;
  }
  for (const char* model : {svp::models::kNomicEmbedTextV15ModelId,
                            svp::models::kNomicEmbedVisionV15ModelId,
                            svp::models::kDepthAnythingV2SmallModelId,
                            svp::models::kPpOcrV6MediumDetectorModelId,
                            svp::models::kPpOcrV6MediumRecognizerModelId}) {
    if (!fs::exists(fs::path(*model_cache) / model / "model.svpmodel.json")) {
      std::cout << "Skipping: " << model << " is not in " << *model_cache << "\n";
      return 0;
    }
  }

  Fixture fixture;
  fixture.model_cache = *model_cache;
  fixture.ffmpeg = ffmpeg;
  fixture.worker = worker;
  fixture.ffmpeg_build = tasks::ffmpeg_build_identity(ffmpeg).value();
  fixture.clip = tasks::write_ocr_calibration_clip(ffmpeg, fixture.scratch.path / "clip").path;
  fixture.cas_root = fixture.scratch.path / "cas";
  exec::CacheResult<exec::CasStore> store = exec::CasStore::at(fixture.cas_root);
  require(static_cast<bool>(store), "cache unavailable");
  fixture.artifacts = std::make_unique<exec::CasTaskArtifactAccess>(
      std::move(store).value(), "bs_dispatched_" + std::to_string(::getpid()));
  fixture.source = fixture.artifacts->put(read_file(fixture.clip), "application/octet-stream",
                                          std::string(tasks::kOcrFrameBatchSourceRole));
  fixture.pp_ocr.model_cache_root = *model_cache;
  fixture.pp_ocr.det_threads = kTestThreads;
  fixture.pp_ocr.rec_threads = kTestThreads;
  fixture.pp_ocr.recognition_parallel_workers = 1;
  fixture.raster = svp::media::compute_canonical_analysis_raster(
      {.stored_width = tasks::kOcrCalibrationFrameWidth,
       .stored_height = tasks::kOcrCalibrationFrameHeight});

  const std::optional<std::string> only = env("SVP_DISPATCH_TEST_PEAK_RSS");
  const auto wanted = [&](const std::string& type) { return !only || *only == type; };
  if (wanted("ocr.crop_batch")) test_crop_tasks(fixture);
  if (!only) test_crop_stage(fixture);
  if (wanted("embed.text_batch")) test_text_tasks(fixture);
  if (wanted("embed.keyframe_batch")) test_keyframe_tasks(fixture);
  if (!only) test_embedding_stage(fixture);
  if (wanted("depth.frame_batch")) test_depth(fixture);
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
