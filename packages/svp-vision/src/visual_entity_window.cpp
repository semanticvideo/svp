#include "svp/vision/visual_entity_window.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/visual_entity_frame_decoder.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace svp::vision {
namespace {

VisualEntityDetectorDiagnostics diagnostics_since(
    const VisualEntityDetectorDiagnostics& before,
    const VisualEntityDetectorDiagnostics& after) {
  return {
      .queries_evaluated = after.queries_evaluated - before.queries_evaluated,
      .confidence_filtered = after.confidence_filtered - before.confidence_filtered,
      .area_filtered = after.area_filtered - before.area_filtered,
      .duplicate_filtered = after.duplicate_filtered - before.duplicate_filtered,
      .cap_filtered = after.cap_filtered - before.cap_filtered,
      .detections_emitted = after.detections_emitted - before.detections_emitted};
}

void apply_planned_identity(std::vector<ColorRasterFrame>& frames,
                            const VisualEntityWindowFrameIdentity& identity) {
  if (identity.frame_catalog != nullptr || identity.planned_frame_ids.empty()) {
    return;
  }
  if (frames.size() > identity.planned_frame_ids.size() ||
      frames.size() > identity.planned_frame_indices.size()) {
    throw std::invalid_argument(
        "visual entity window decoded more frames than it has planned IDs");
  }
  // Frames are read in timestamp order and stop at the first short read, so
  // decoded frame i is the window's timestamp i.
  for (std::size_t index = 0; index < frames.size(); ++index) {
    frames[index].frame_id = identity.planned_frame_ids[index];
    frames[index].frame_index = identity.planned_frame_indices[index];
  }
}

}  // namespace

VisualEntityPipelinePlan plan_visual_entity_pipeline(
    const media::MediaIngestPlan& media_plan,
    const VisualEntityPipelineOptions& options) {
  const auto quality_policy = visual_tracking_quality_policy(options.quality);
  VisualEntityPipelinePlan plan;
  plan.sampling = visual_entity_sampling_options(quality_policy);
  plan.depth_schedule = options.depth_schedule;
  plan.depth_schedule.periodic_interval_us = quality_policy.depth_interval_us;
  if (plan.depth_schedule.periodic_interval_us < plan.sampling.sample_interval_us ||
      plan.depth_schedule.periodic_interval_us % plan.sampling.sample_interval_us != 0) {
    throw std::invalid_argument(
        "visual entity depth cadence must be an integer multiple of the RGB cadence");
  }
  plan.duration_us = compute_media_duration_us(media_plan);
  plan.windows = make_visual_entity_sampling_plan(plan.duration_us, plan.sampling);
  return plan;
}

VisualEntityDetectorOptions visual_entity_window_detector_options(
    const VisualEntityPipelineOptions& options) {
  VisualEntityDetectorOptions detector = options.detector;
  detector.execution_provider = options.execution_provider;
  return detector;
}

VisualEntityWindowRuntimes load_visual_entity_window_runtimes(
    const std::filesystem::path& model_cache_root,
    const VisualEntityPipelineOptions& options) {
  // Load order is the stage's: embedding, depth, detector.
  VisualEntityWindowRuntimes runtimes;
  runtimes.embedding = load_visual_entity_embedding_runtime(
      model_cache_root, options.embedding_model_id, options.execution_provider,
      options.embedding_threads);
  runtimes.depth = load_depth_inference_runtime(
      model_cache_root, svp::models::kDepthAnythingV2SmallModelId,
      options.execution_provider, options.depth_threads);
  runtimes.detector = load_visual_entity_detector(
      model_cache_root, visual_entity_window_detector_options(options));
  return runtimes;
}

bool visual_entity_window_runtimes_complete(const VisualEntityWindowRuntimes& runtimes) {
  return runtimes.detector.session != nullptr && runtimes.depth.session != nullptr &&
         runtimes.embedding.session != nullptr;
}

VisualEntityWindowRuntimeStatus visual_entity_window_runtime_status(
    const VisualEntityWindowRuntimes& runtimes) {
  return {.detector_loaded = runtimes.detector.session != nullptr,
          .detector_blocker = runtimes.detector.blocker,
          .detector_model_refs = runtimes.detector.model_refs,
          .detector_model_identity = runtimes.detector.model_identity,
          .depth_loaded = runtimes.depth.session != nullptr,
          .depth_blocker = runtimes.depth.blocker};
}

VisualEntityWindowRequest make_visual_entity_window_request(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    const VisualEntityPipelineOptions& options,
    const VisualEntityPipelinePlan& plan,
    std::size_t window_index) {
  return {.source_path = media_plan.source_path,
          .ffmpeg_path = ffmpeg_path,
          .frame_width = media_plan.canonical_raster.width,
          .frame_height = media_plan.canonical_raster.height,
          .window = plan.windows.at(window_index),
          .depth_schedule = plan.depth_schedule,
          .cut_detection = options.cut_detection,
          .embedding_model_id = options.embedding_model_id,
          .execution_provider = options.execution_provider};
}

VisualEntityWindowOutcome run_visual_entity_window(
    const VisualEntityWindowRequest& request,
    VisualEntityWindowRuntimes& runtimes,
    const VisualEntityWindowFrameIdentity& identity,
    const VisualEntityWindowCheckpoint& checkpoint) {
  const auto check = [&checkpoint] {
    if (checkpoint) checkpoint();
  };
  VisualEntityWindowOutcome outcome;
  outcome.runtime_status = visual_entity_window_runtime_status(runtimes);
  const VisualEntityDetectorDiagnostics diagnostics_before =
      runtimes.detector.diagnostics;
  media::MediaIngestPlan decode_plan;
  decode_plan.source_path = request.source_path;
  auto decoded = decode_visual_entity_window(
      decode_plan, request.ffmpeg_path, request.frame_width, request.frame_height,
      request.window.timestamps_us, identity.frame_catalog,
      kVisualEntityTrackingFramePurpose);
  apply_planned_identity(decoded.frames, identity);
  outcome.frames_attempted = static_cast<std::size_t>(decoded.frames_attempted);
  outcome.frames_decoded = static_cast<std::size_t>(decoded.frames_decoded);
  outcome.frames_missed = static_cast<std::size_t>(decoded.frames_missed);
  for (const auto& frame : decoded.frames) {
    outcome.decoded_timestamps_us.push_back(frame.timestamp_us);
  }

  if (!decoded.decoding_succeeded || decoded.frames.size() < 2) {
    outcome.status = VisualEntityWindowStatus::decode_failed;
    outcome.failures.push_back(
        {"frame_decode", decoded.decoding_succeeded
                             ? "fewer than two visual entity frames were decoded"
                             : decoded.skipped_reason});
    return outcome;
  }

  std::vector<std::uint16_t> window_depth;
  std::vector<std::string> depth_frame_ids;
  const auto proposal_frame_indices =
      select_visual_entity_depth_frames(decoded.frames, request.depth_schedule);
  std::vector<ExternalEntityProposal> detector_proposals;
  if (runtimes.detector.session) {
    for (const auto& frame : decoded.frames) {
      check();
      try {
        for (const auto& detection : detect_visual_entities(runtimes.detector, frame)) {
          ExternalEntityProposal proposal;
          proposal.frame_id = frame.frame_id;
          std::copy(std::begin(detection.box_px), std::end(detection.box_px),
                    std::begin(proposal.box_px));
          proposal.confidence = detection.confidence;
          proposal.detector_category_index = detection.category_index;
          proposal.source = "objectness_detector";
          detector_proposals.push_back(std::move(proposal));
        }
      } catch (const std::exception& error) {
        outcome.failures.push_back({"objectness_detector", error.what()});
      }
    }
  }
  outcome.detector_diagnostics =
      diagnostics_since(diagnostics_before, runtimes.detector.diagnostics);
  if (runtimes.depth.session) {
    const std::size_t depth_values_per_frame =
        static_cast<std::size_t>(request.frame_width) * request.frame_height;
    window_depth.reserve(proposal_frame_indices.size() * depth_values_per_frame);
    for (const auto frame_index : proposal_frame_indices) {
      check();
      const auto& frame = decoded.frames[frame_index];
      try {
        auto frame_depth = infer_depth_frame(runtimes.depth, frame);
        if (frame_depth.size() != depth_values_per_frame) {
          outcome.failures.push_back(
              {"depth_inference",
               "depth output dimensions did not match the canonical raster"});
          window_depth.clear();
          depth_frame_ids.clear();
          break;
        }
        window_depth.insert(window_depth.end(), frame_depth.begin(), frame_depth.end());
        depth_frame_ids.push_back(frame.frame_id);
      } catch (const std::exception& error) {
        outcome.failures.push_back({"depth_inference", error.what()});
        window_depth.clear();
        depth_frame_ids.clear();
        break;
      }
    }
  }

  VisualEntityTrackerOptions tracker_options;
  // The dedicated sampling plan already owns temporal selection. Every
  // decoded observation must reach tracking; a second frame-count sampler
  // would recreate the five-frame coverage bug.
  tracker_options.keyframe_interval_frames = 1;
  tracker_options.embedding_model_id = request.embedding_model_id;
  tracker_options.execution_provider = request.execution_provider;
  tracker_options.embedding_runtime = &runtimes.embedding;
  tracker_options.external_proposals = std::move(detector_proposals);

  outcome.cut_evidence = detect_visual_entity_cuts(decoded.frames, request.cut_detection);
  std::vector<std::pair<std::string, std::int64_t>> cut_boundaries;
  for (const auto& evidence : outcome.cut_evidence) {
    if (evidence.is_cut) {
      cut_boundaries.emplace_back("visual_cut", evidence.timestamp_us);
      outcome.cut_timestamps_us.push_back(evidence.timestamp_us);
    }
  }

  check();
  try {
    // The tracker reads no model cache when an embedding runtime is given.
    outcome.tracker_result = run_visual_entity_tracker(
        decoded.frames, window_depth, depth_frame_ids, cut_boundaries,
        std::filesystem::path(), tracker_options);
  } catch (const std::exception& error) {
    outcome.failures.push_back({"visual_entity_tracker", error.what()});
    outcome.status = VisualEntityWindowStatus::tracker_failed;
    return outcome;
  }
  outcome.status = VisualEntityWindowStatus::tracked;
  return outcome;
}

}  // namespace svp::vision
