// track.window without models: parameters round-trip and the validator
// refuses anything a worker could fill from its own host or that does not
// describe one window; specs name the planned frame IDs and scale their
// resource estimates with the window's frames and raster; a window that
// cannot start is a not_started outcome on the coordinator and a retryable
// failure on a worker.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/task_registry.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"
#include "svp/vision/tasks/track_window_spec.hpp"
#include "svp/vision/tasks/track_window_task.hpp"
#include "svp/vision/visual_entity_window_codec.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace svp::vision;
using namespace svp::vision::tasks;

int failures = 0;

void check(bool condition, const std::string& message) {
  if (condition) return;
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

VisualEntityPipelineOptions options_for(VisualTrackingQuality quality) {
  VisualEntityPipelineOptions options;
  options.quality = quality;
  options.detector.threads = {.intra_op = 4, .inter_op = 1};
  options.depth_threads = {.intra_op = 3, .inter_op = 1};
  options.embedding_threads = {.intra_op = 2, .inter_op = 1};
  return options;
}

VisualEntityPipelinePlan plan_for(VisualTrackingQuality quality, std::int64_t duration_us) {
  const auto policy = visual_tracking_quality_policy(quality);
  VisualEntityPipelinePlan plan;
  plan.sampling = visual_entity_sampling_options(policy);
  plan.depth_schedule.periodic_interval_us = policy.depth_interval_us;
  plan.duration_us = duration_us;
  plan.windows = make_visual_entity_sampling_plan(duration_us, plan.sampling);
  return plan;
}

FrameCatalog catalog_for(const VisualEntityPipelinePlan& plan) {
  FrameCatalog catalog;
  // Frames of earlier stages come first, as in a build's frame plan.
  (void)catalog.register_frame(-1, kColorFramePurpose);
  for (const auto& window : plan.windows) {
    for (const std::int64_t timestamp : window.timestamps_us) {
      (void)catalog.register_frame(timestamp, kVisualEntityTrackingFramePurpose);
    }
  }
  catalog.lock_to_plan();
  return catalog;
}

svp::exec::TaskModelRef fake_ref(const std::string& model_id, std::uint8_t fill) {
  svp::exec::Blake3Digest digest{};
  digest.fill(fill);
  return svp::exec::TaskModelRef{
      .model_id = model_id,
      .model_bundle_id = model_id + "@test+blake3_" + svp::exec::blake3_hex(digest).substr(0, 12),
      .bundle_blake3 = digest};
}

TrackWindowTaskInputs inputs_for(const VisualEntityPipelineOptions& options, int width,
                                 int height) {
  svp::exec::Blake3Digest source{};
  source.fill(0x11);
  return TrackWindowTaskInputs{
      .build_session_id = "bs_test",
      .depends_on = {"task.b", "task.a"},
      .source = {.blake3 = source,
                 .bytes = 4096,
                 .media_type = "application/octet-stream",
                 .role = std::string(kTrackWindowSourceRole)},
      .model_refs = {fake_ref(options.detector.model_id, 0x21),
                     fake_ref(track_window_depth_model_id(), 0x22),
                     fake_ref(options.embedding_model_id, 0x23)},
      .options = options,
      .frame_width = width,
      .frame_height = height,
      .ffmpeg_build = "b3:" + std::string(64, 'c'),
      .cost = {.estimated_seconds_per_frame = 0.5},
  };
}

bool rejects(nlohmann::json parameters) {
  return validate_track_window_parameters(parameters).has_value();
}

void test_parameters() {
  const auto options = options_for(VisualTrackingQuality::medium);
  const auto plan = plan_for(VisualTrackingQuality::medium, 45'000'000);
  const FrameCatalog catalog = catalog_for(plan);
  const svp::exec::TaskSpec spec =
      make_track_window_task_spec(inputs_for(options, 640, 360), plan, 1, catalog);
  const nlohmann::json& value = spec.parameters;
  const TrackWindowParameters parameters = track_window_parameters_from_json(value);
  check(track_window_parameters_to_json(parameters) == value, "parameters round-trip");
  check(parameters.window.timestamps_us == plan.windows[1].timestamps_us,
        "the spec carries its window's timestamps");
  for (std::size_t index = 0; index < parameters.frame_ids.size(); ++index) {
    const std::size_t planned = *catalog.get_frame_index(parameters.window.timestamps_us[index]);
    check(parameters.frame_ids[index] == catalog.planned_entries()[planned].frame_id &&
              parameters.frame_indices[index] == planned,
          "frame " + std::to_string(index) + " carries its planned ID");
  }

  nlohmann::json bad = value;
  bad["detector"]["threads"]["intra_op"] = 0;
  check(rejects(bad), "a thread count the worker would choose is refused");
  bad = value;
  bad["extra"] = 1;
  check(rejects(bad), "an unknown field is refused");
  bad = value;
  bad["window"]["frame_ids"].erase(bad["window"]["frame_ids"].begin());
  check(rejects(bad), "frame IDs that do not pair with timestamps are refused");
  bad = value;
  std::swap(bad["window"]["timestamps_us"][0], bad["window"]["timestamps_us"][1]);
  check(rejects(bad), "timestamps out of order are refused");
  bad = value;
  bad["window"]["timestamps_us"].back() = bad["window"]["end_us"].get<std::int64_t>() + 1;
  check(rejects(bad), "a timestamp outside the window is refused");
  bad = value;
  bad["depth"]["model_id"] = "model_other_depth";
  check(rejects(bad), "a depth model the window does not load is refused");
  bad = value;
  bad["decode"]["frame_width"] = svp::media::kCanonicalLongestDisplayDimension + 1;
  check(rejects(bad), "a decode size beyond the canonical raster is refused");
  bad = value;
  bad["detector"]["confidence_threshold"] = 1;
  check(rejects(bad), "an integer where the encoder writes a float is refused");
}

// Specs scale with the plan: one per window, whatever the duration or
// cadence, with estimates that grow with frames and raster.
void test_specs_follow_the_plan() {
  for (const VisualTrackingQuality quality :
       {VisualTrackingQuality::low, VisualTrackingQuality::medium, VisualTrackingQuality::high}) {
    for (const std::int64_t duration_us : {6'000'000LL, 61'000'000LL, 7'260'000'000LL}) {
      const auto options = options_for(quality);
      const auto plan = plan_for(quality, duration_us);
      const FrameCatalog catalog = catalog_for(plan);
      std::map<std::string, int> ids;
      for (std::size_t index = 0; index < plan.windows.size(); ++index) {
        const svp::exec::TaskSpec spec =
            make_track_window_task_spec(inputs_for(options, 640, 360), plan, index, catalog);
        ++ids[spec.task_id];
        const std::size_t frames = plan.windows[index].timestamps_us.size();
        check(spec.resources.est_seconds >= frames / 2,
              "estimated seconds follow the window's frames");
      }
      check(ids.size() == plan.windows.size(), "one distinct task per window");
    }
  }
  const auto options = options_for(VisualTrackingQuality::medium);
  const auto plan = plan_for(VisualTrackingQuality::medium, 45'000'000);
  const FrameCatalog catalog = catalog_for(plan);
  const auto small = make_track_window_task_spec(inputs_for(options, 320, 180), plan, 0, catalog);
  const auto large = make_track_window_task_spec(inputs_for(options, 640, 360), plan, 0, catalog);
  const auto dense_plan = plan_for(VisualTrackingQuality::high, 45'000'000);
  const auto dense = make_track_window_task_spec(inputs_for(options, 640, 360), dense_plan, 0,
                                                 catalog_for(dense_plan));
  check(small.resources.est_peak_rss_mb < large.resources.est_peak_rss_mb,
        "a larger raster is estimated to need more memory");
  check(large.resources.est_peak_rss_mb < dense.resources.est_peak_rss_mb,
        "a denser window is estimated to need more memory");
  check(large.resources.est_peak_rss_mb >= kTrackWindowRuntimeResidentMb,
        "the estimate covers the loaded runtimes");
  check(large.resources.est_peak_rss_mb ==
            track_window_estimated_peak_rss_mb(plan.windows[0].timestamps_us.size(), 640, 360,
                                               options.detector.maximum_detections),
        "a coordinator sizing its slots for a window uses the spec's own estimate");
  check(track_window_task_id(7) == "task.tracking.vstream_000.window_000007",
        "window task IDs are stable");
  check(large.depends_on == std::vector<std::string>{"task.a", "task.b"}, "deps are sorted");
  bool rejected = false;
  try {
    (void)make_track_window_task_spec(inputs_for(options, 640, 360), plan, plan.windows.size(),
                                      catalog);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "a window outside the plan has no spec");
}

void test_start_failures() {
  const auto options = options_for(VisualTrackingQuality::medium);
  const auto plan = plan_for(VisualTrackingQuality::medium, 45'000'000);
  const FrameCatalog catalog = catalog_for(plan);
  const svp::exec::TaskSpec spec =
      make_track_window_task_spec(inputs_for(options, 640, 360), plan, 0, catalog);
  for (const bool coordinator : {true, false}) {
    std::vector<std::byte> written;
    svp::exec::TaskTypeRegistry registry;
    register_track_window_task(
        registry,
        TrackWindowWorkerEnvironment{
            .model_cache_root = "/nonexistent/models",
            .ffmpeg_path = "/nonexistent/ffmpeg",
            .write_output =
                [&written](std::span<const std::byte> bytes, std::string media_type,
                           std::string role) {
                  written.assign(bytes.begin(), bytes.end());
                  return svp::exec::make_artifact_ref(bytes, std::move(media_type),
                                                      std::move(role));
                },
            .model_cache_for = {},
            .record_start_failures = coordinator});
    svp::exec::ResolvedInputs inputs;
    inputs.emplace(std::string(kTrackWindowSourceInput),
                   svp::exec::ResolvedInput{.ref = spec.inputs.at("source"),
                                            .path = "/nonexistent/source.mp4"});
    const svp::exec::CancellationToken cancellation;
    const svp::exec::TaskResult result = registry.execute(spec, inputs, cancellation);
    if (coordinator) {
      check(result.status == svp::exec::TaskStatus::succeeded,
            "on the coordinator a window that cannot start succeeds");
      const VisualEntityWindowOutcome outcome = decode_visual_entity_window_outcome(
          std::span(reinterpret_cast<const std::uint8_t*>(written.data()), written.size()));
      check(outcome.status == VisualEntityWindowStatus::not_started,
            "its outcome is not_started, so the stage runs as without window tasks");
    } else {
      check(result.status == svp::exec::TaskStatus::failed && result.error &&
                result.error->retryable,
            "on a worker a window that cannot start is a retryable failure");
    }
  }
}

// With the models (SVP_MODEL_CACHE_ROOT) and a decoder that runs but cannot
// decode: on the coordinator the window's decode failure is recorded as a
// local build records it; on a worker the same window is a retryable failure
// (window_failed_here), so it runs on another Mac instead of putting this
// Mac's trouble into the package. Skips without the models.
void test_window_trouble_on_a_worker_is_retried() {
  const char* models = std::getenv("SVP_MODEL_CACHE_ROOT");
  if (models == nullptr) {
    std::cout << "skipping window trouble test: SVP_MODEL_CACHE_ROOT is not set\n";
    return;
  }
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "svp-track-window-trouble-test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path ffmpeg = directory / "ffmpeg";
  {
    std::ofstream script(ffmpeg);
    script << "#!/bin/sh\n"
              "if [ \"$1\" = -version ]; then echo 'ffmpeg version trouble-test'; exit 0; fi\n"
              "exit 1\n";
  }
  std::filesystem::permissions(ffmpeg, std::filesystem::perms::owner_all);
  const std::optional<std::string> build = ffmpeg_build_identity(ffmpeg);
  check(build.has_value(), "the stand-in decoder reports a build");

  const auto options = options_for(VisualTrackingQuality::medium);
  const auto plan = plan_for(VisualTrackingQuality::medium, 45'000'000);
  const FrameCatalog catalog = catalog_for(plan);
  TrackWindowTaskInputs inputs = inputs_for(options, 640, 360);
  inputs.model_refs = track_window_model_refs(models, options);
  inputs.ffmpeg_build = build.value_or("");
  const svp::exec::TaskSpec spec = make_track_window_task_spec(inputs, plan, 0, catalog);
  auto runtimes = std::make_shared<TrackWindowRuntimePool>();
  for (const bool coordinator : {true, false}) {
    std::vector<std::byte> written;
    svp::exec::TaskTypeRegistry registry;
    register_track_window_task(
        registry,
        TrackWindowWorkerEnvironment{
            .model_cache_root = models,
            .ffmpeg_path = ffmpeg,
            .write_output =
                [&written](std::span<const std::byte> bytes, std::string media_type,
                           std::string role) {
                  written.assign(bytes.begin(), bytes.end());
                  return svp::exec::make_artifact_ref(bytes, std::move(media_type),
                                                      std::move(role));
                },
            .model_cache_for = {},
            .record_start_failures = coordinator},
        runtimes);
    svp::exec::ResolvedInputs resolved;
    resolved.emplace(std::string(kTrackWindowSourceInput),
                     svp::exec::ResolvedInput{.ref = spec.inputs.at("source"),
                                              .path = directory / "source.mp4"});
    const svp::exec::CancellationToken cancellation;
    const svp::exec::TaskResult result = registry.execute(spec, resolved, cancellation);
    if (coordinator) {
      check(result.status == svp::exec::TaskStatus::succeeded,
            "on the coordinator a window that does not decode is a result");
      const VisualEntityWindowOutcome outcome = decode_visual_entity_window_outcome(
          std::span(reinterpret_cast<const std::uint8_t*>(written.data()), written.size()));
      check(outcome.status == VisualEntityWindowStatus::decode_failed &&
                !outcome.failures.empty() && outcome.failures.front().component == "frame_decode",
            "its decode failure is recorded as a local build records it");
    } else {
      check(result.status == svp::exec::TaskStatus::failed && result.error &&
                result.error->retryable && result.error->code == "window_failed_here",
            "on a worker the same window is a retryable failure");
    }
  }
  std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
  test_parameters();
  test_specs_follow_the_plan();
  test_start_failures();
  test_window_trouble_on_a_worker_is_retried();
  if (failures != 0) return 1;
  std::cout << "All track.window task tests passed.\n";
  return 0;
}
