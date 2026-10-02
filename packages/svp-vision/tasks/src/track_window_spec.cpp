#include "svp/vision/tasks/track_window_spec.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/cache_key.hpp"
#include "svp/exec/parameters_digest.hpp"
#include "svp/models/manifest.hpp"
#include "svp/vision/tasks/track_window_parameters.hpp"
#include "svp/vision/visual_entity_window_codec.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace svp::vision::tasks {
namespace {

constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;
constexpr const char* kManifestFilename = "model.svpmodel.json";

svp::exec::TaskModelRef model_ref(const std::filesystem::path& model_cache_root,
                                  const std::string& model_id) {
  const std::filesystem::path manifest_path = model_cache_root / model_id / kManifestFilename;
  const svp::models::ModelBundleManifest manifest = [&] {
    try {
      return svp::models::load_model_bundle_manifest(manifest_path);
    } catch (const std::exception& error) {
      throw std::runtime_error("track.window model ref for " + model_id + ": " + error.what());
    }
  }();
  const auto bundle_blake3 = svp::exec::parse_blake3_hex(manifest.bundle_blake3.hex_value());
  if (manifest.model_id != model_id || !bundle_blake3) {
    throw std::runtime_error("track.window model ref for " + model_id + ": manifest " +
                             manifest_path.string() + " does not identify that model by BLAKE3");
  }
  return svp::exec::TaskModelRef{.model_id = manifest.model_id,
                                 .model_bundle_id = manifest.model_bundle_id,
                                 .bundle_blake3 = *bundle_blake3};
}

std::string padded_ordinal(std::uint64_t ordinal) {
  std::ostringstream oss;
  oss << std::setw(6) << std::setfill('0') << ordinal;
  return oss.str();
}

std::uint64_t estimated_peak_rss_mb(const TrackWindowParameters& parameters) {
  return track_window_estimated_peak_rss_mb(parameters.window.timestamps_us.size(),
                                            parameters.frame_width, parameters.frame_height,
                                            parameters.detector.maximum_detections);
}

// The window's phases (detection, depth, embedding) run one after another,
// so the busiest uses about the largest pool.
std::uint64_t estimated_cpu_threads(const TrackWindowParameters& parameters) {
  return static_cast<std::uint64_t>(std::max({1, parameters.detector.threads.intra_op,
                                              parameters.depth_threads.intra_op,
                                              parameters.embedding_threads.intra_op}));
}

std::uint64_t estimated_seconds(const TrackWindowCostPolicy& cost, std::size_t frames) {
  if (!(cost.estimated_seconds_per_frame > 0.0) ||
      !std::isfinite(cost.estimated_seconds_per_frame)) {
    throw std::invalid_argument("track.window cost needs a positive seconds per frame");
  }
  const double seconds = std::ceil(static_cast<double>(frames) * cost.estimated_seconds_per_frame);
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(seconds));
}

}  // namespace

std::uint64_t track_window_estimated_peak_rss_mb(std::size_t frames, int width, int height,
                                                 std::size_t maximum_detections) {
  const std::uint64_t pixels = static_cast<std::uint64_t>(std::max(width, 0)) *
                               static_cast<std::uint64_t>(std::max(height, 0));
  const std::uint64_t bytes_per_pixel = kTrackWindowFrameBytesPerPixel + maximum_detections;
  const std::uint64_t window_bytes = frames * pixels * bytes_per_pixel;
  return kTrackWindowRuntimeResidentMb + (window_bytes + kBytesPerMiB - 1) / kBytesPerMiB;
}

std::vector<svp::exec::TaskModelRef> track_window_model_refs(
    const std::filesystem::path& model_cache_root, const VisualEntityPipelineOptions& options) {
  std::vector<svp::exec::TaskModelRef> refs = {
      model_ref(model_cache_root, options.detector.model_id),
      model_ref(model_cache_root, track_window_depth_model_id()),
      model_ref(model_cache_root, options.embedding_model_id),
  };
  std::sort(refs.begin(), refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  return refs;
}

std::string track_window_task_id(std::size_t window_index) {
  return std::string(kTrackWindowTaskIdPrefix) + padded_ordinal(window_index);
}

svp::exec::TaskOrderKey track_window_order_key(std::size_t window_index) {
  return svp::exec::TaskOrderKey{.lane = std::string(kTrackWindowLane),
                                 .ordinals = {static_cast<std::uint64_t>(window_index)}};
}

svp::exec::TaskSpec make_track_window_task_spec(const TrackWindowTaskInputs& inputs,
                                                const VisualEntityPipelinePlan& plan,
                                                std::size_t window_index,
                                                const FrameCatalog& planned_catalog) {
  if (window_index >= plan.windows.size()) {
    throw std::invalid_argument("track.window window is outside the tracking plan");
  }
  TrackWindowParameters parameters;
  parameters.window_ordinal = window_index;
  parameters.window = plan.windows[window_index];
  for (const std::int64_t timestamp_us : parameters.window.timestamps_us) {
    const auto index = planned_catalog.get_frame_index(timestamp_us);
    if (!index || *index >= planned_catalog.planned_entries().size()) {
      throw std::invalid_argument("track.window timestamp " + std::to_string(timestamp_us) +
                                  " us is not in the planned frame catalog");
    }
    parameters.frame_ids.push_back(planned_catalog.planned_entries()[*index].frame_id);
    parameters.frame_indices.push_back(*index);
  }
  parameters.frame_width = inputs.frame_width;
  parameters.frame_height = inputs.frame_height;
  parameters.ffmpeg_build = inputs.ffmpeg_build;
  parameters.depth_schedule = plan.depth_schedule;
  parameters.cut_detection = inputs.options.cut_detection;
  parameters.detector = visual_entity_window_detector_options(inputs.options);
  parameters.depth_threads = inputs.options.depth_threads;
  parameters.embedding_threads = inputs.options.embedding_threads;
  parameters.embedding_model_id = inputs.options.embedding_model_id;
  parameters.execution_provider = inputs.options.execution_provider;

  svp::exec::TaskSpec spec;
  spec.build_session_id = inputs.build_session_id;
  spec.task_id = track_window_task_id(window_index);
  spec.task_type = std::string(kTrackWindowTaskType);
  spec.task_type_version = kTrackWindowTaskTypeVersion;
  spec.depends_on = inputs.depends_on;
  std::sort(spec.depends_on.begin(), spec.depends_on.end());
  spec.model_refs = inputs.model_refs;
  std::sort(spec.model_refs.begin(), spec.model_refs.end(),
            [](const auto& left, const auto& right) { return left.model_id < right.model_id; });
  spec.inputs.emplace(std::string(kTrackWindowSourceInput), inputs.source);
  spec.parameters = track_window_parameters_to_json(parameters);
  spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);

  nlohmann::json bundle_ids = nlohmann::json::array();
  for (const svp::exec::TaskModelRef& ref : spec.model_refs) {
    bundle_ids.push_back(ref.model_bundle_id);
  }
  spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array({
      spec.task_type,
      spec.task_type_version,
      svp::exec::blake3_hex(inputs.source.blake3),
      std::move(bundle_ids),
      svp::exec::blake3_hex(spec.parameters_blake3),
  }));
  spec.resources = svp::exec::TaskResources{
      .est_peak_rss_mb = estimated_peak_rss_mb(parameters),
      .est_cpu_threads = estimated_cpu_threads(parameters),
      .est_seconds = estimated_seconds(inputs.cost, parameters.window.timestamps_us.size()),
  };
  svp::exec::validate_task_spec(spec);
  return spec;
}

VisualEntityWindowOutcome read_track_window_output(const svp::exec::TaskSpec& spec,
                                                   std::span<const std::uint8_t> payload) {
  const TrackWindowParameters parameters = track_window_parameters_from_json(spec.parameters);
  VisualEntityWindowOutcome outcome = decode_visual_entity_window_outcome(payload);
  const auto& planned = parameters.window.timestamps_us;
  const auto& decoded = outcome.decoded_timestamps_us;
  if (decoded.size() > planned.size() ||
      !std::equal(decoded.begin(), decoded.end(), planned.begin())) {
    throw std::invalid_argument(spec.task_id +
                                ": decoded frames are not a prefix of the window's timestamps");
  }
  if (outcome.frames_attempted != 0 && outcome.frames_attempted != planned.size()) {
    throw std::invalid_argument(spec.task_id + ": output attempted " +
                                std::to_string(outcome.frames_attempted) + " of " +
                                std::to_string(planned.size()) + " frames");
  }
  return outcome;
}

}  // namespace svp::vision::tasks
