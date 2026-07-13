#include "svp/vision/visual_entity_pipeline.hpp"

#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/depth_generation.hpp"
#include "svp/vision/visual_entity_frame_decoder.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace svp::vision {

VisualEntityPipelineResult run_visual_entity_pipeline(
    const media::MediaIngestPlan& media_plan,
    const std::filesystem::path& ffmpeg_path,
    const std::filesystem::path& model_cache_root,
    const std::vector<std::pair<std::string, std::int64_t>>& shot_boundaries,
    FrameCatalog* frame_catalog,
    const VisualEntityPipelineOptions& options) {
  VisualEntityPipelineResult result;
  // The package's current shot timeline is one range per foundation frame,
  // not a cinematic-cut contract. Entity tracking derives cut boundaries
  // from its own dense window frames instead.
  (void)shot_boundaries;
  if (options.depth_schedule.periodic_interval_us <
          options.sampling.sample_interval_us ||
      options.depth_schedule.periodic_interval_us %
              options.sampling.sample_interval_us != 0) {
    throw std::invalid_argument(
        "visual entity depth cadence must be an integer multiple of the RGB cadence");
  }
  const std::int64_t duration_us = compute_media_duration_us(media_plan);
  const auto windows =
      make_visual_entity_sampling_plan(duration_us, options.sampling);
  result.windows_planned = windows.size();
  if (windows.empty()) {
    result.blocker = "video duration is unavailable for visual entity tracking";
    return result;
  }

  VisualEntityWindowAssembler assembler(options.assembly);
  auto embedding_runtime = load_visual_entity_embedding_runtime(
      model_cache_root,
      options.embedding_model_id,
      options.execution_provider);
  auto depth_runtime = load_depth_inference_runtime(
      model_cache_root,
      "model_depth_anything_v2_small",
      options.execution_provider);
  auto detector_options = options.detector;
  detector_options.execution_provider = options.execution_provider;
  auto detector_runtime = load_visual_entity_detector(
      model_cache_root, detector_options);
  std::int64_t previous_window_end_us = -1;
  if (options.on_progress) options.on_progress(0, windows.size());

  for (std::size_t window_index = 0;
       window_index < windows.size(); ++window_index) {
    const auto& window = windows[window_index];
    auto decoded = decode_visual_entity_window(
        media_plan,
        ffmpeg_path,
        media_plan.canonical_raster.width,
        media_plan.canonical_raster.height,
        window.timestamps_us,
        frame_catalog,
        "visual_entity_tracking");
    result.frames_attempted += decoded.frames_attempted;
    result.frames_decoded += decoded.frames_decoded;
    result.frames_missed += decoded.frames_missed;

    if (decoded.decoding_succeeded && decoded.frames.size() >= 2) {
      std::vector<std::uint16_t> window_depth;
      std::vector<std::string> depth_frame_ids;
      const auto proposal_frame_indices = select_visual_entity_depth_frames(
          decoded.frames, options.depth_schedule);
      std::vector<ExternalEntityProposal> detector_proposals;
      if (detector_runtime.session) {
        for (const auto& frame : decoded.frames) {
          try {
            for (const auto& detection :
                 detect_visual_entities(detector_runtime, frame)) {
              ExternalEntityProposal proposal;
              proposal.frame_id = frame.frame_id;
              std::copy(std::begin(detection.box_px),
                        std::end(detection.box_px),
                        std::begin(proposal.box_px));
              proposal.confidence = detection.confidence;
              proposal.detector_category_index = detection.category_index;
              proposal.source = "objectness_detector";
              detector_proposals.push_back(std::move(proposal));
            }
          } catch (...) {
          }
        }
      }
      if (depth_runtime.session) {
        const auto& depth_frame_indices = proposal_frame_indices;
        const std::size_t depth_values_per_frame =
            static_cast<std::size_t>(media_plan.canonical_raster.width) *
            media_plan.canonical_raster.height;
        window_depth.reserve(
            depth_frame_indices.size() * depth_values_per_frame);
        for (const auto frame_index : depth_frame_indices) {
          const auto& frame = decoded.frames[frame_index];
          try {
            auto frame_depth = infer_depth_frame(depth_runtime, frame);
            if (frame_depth.size() != depth_values_per_frame) {
              window_depth.clear();
              depth_frame_ids.clear();
              break;
            }
            window_depth.insert(
                window_depth.end(), frame_depth.begin(), frame_depth.end());
            depth_frame_ids.push_back(frame.frame_id);
          } catch (...) {
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
      tracker_options.embedding_model_id = options.embedding_model_id;
      tracker_options.execution_provider = options.execution_provider;
      tracker_options.embedding_runtime = &embedding_runtime;
      tracker_options.external_proposals = std::move(detector_proposals);

      const auto window_cut_evidence = detect_visual_entity_cuts(
          decoded.frames, options.cut_detection);
      std::vector<std::pair<std::string, std::int64_t>> cut_boundaries;
      std::vector<std::int64_t> cut_timestamps_us;
      for (const auto& evidence : window_cut_evidence) {
        if (evidence.is_cut) {
          cut_boundaries.emplace_back("visual_cut", evidence.timestamp_us);
          cut_timestamps_us.push_back(evidence.timestamp_us);
        }
        const bool already_recorded = std::any_of(
            result.cut_evidence.begin(), result.cut_evidence.end(),
            [&](const auto& existing) {
              return existing.timestamp_us == evidence.timestamp_us;
            });
        if (!already_recorded) result.cut_evidence.push_back(evidence);
      }

      auto window_result = run_visual_entity_tracker(
          decoded.frames,
          window_depth,
          depth_frame_ids,
          cut_boundaries,
          model_cache_root,
          tracker_options);
      window_result.model_refs.insert(
          window_result.model_refs.end(),
          detector_runtime.model_refs.begin(),
          detector_runtime.model_refs.end());
      std::sort(window_result.model_refs.begin(), window_result.model_refs.end());
      window_result.model_refs.erase(
          std::unique(window_result.model_refs.begin(),
                      window_result.model_refs.end()),
          window_result.model_refs.end());
      if (!detector_runtime.session && !detector_runtime.blocker.empty()) {
        window_result.limitations_note +=
            " Objectness detector unavailable: " +
            detector_runtime.blocker + ".";
      }
      if (!depth_runtime.session && !depth_runtime.blocker.empty()) {
        window_result.limitations_note +=
            " Depth support unavailable: " + depth_runtime.blocker + ".";
      }
      window_result.parameters_json["visual_entity_sampling"] = {
          {"sample_interval_us", options.sampling.sample_interval_us},
          {"window_duration_us", options.sampling.window_duration_us},
          {"window_overlap_us", options.sampling.window_overlap_us}};
      window_result.parameters_json["objectness_detector"] = {
          {"model_id", options.detector.model_id},
          {"confidence_threshold", options.detector.confidence_threshold},
          {"category_evidence_confidence_threshold",
           options.detector.category_evidence_confidence_threshold},
          {"nms_iou_threshold", options.detector.nms_iou_threshold},
          {"nms_containment_threshold",
           options.detector.nms_containment_threshold},
          {"cross_category_duplicate_iou_threshold",
           options.detector.cross_category_duplicate_iou_threshold}};
      window_result.parameters_json["cut_detection"] = {
          {"immediate_difference_threshold",
           options.cut_detection.immediate_difference_threshold},
          {"stable_difference_threshold",
           options.cut_detection.stable_difference_threshold},
          {"hard_cut_difference_threshold",
           options.cut_detection.hard_cut_difference_threshold},
          {"sustained_transition_difference_threshold",
           options.cut_detection.sustained_transition_difference_threshold},
          {"sustained_transition_observations",
           options.cut_detection.sustained_transition_observations}};
      assembler.append_window(
          std::move(window_result),
          window.timestamps_us,
          window.start_us,
          previous_window_end_us,
          cut_timestamps_us);
    }

    previous_window_end_us = window.end_us;
    ++result.windows_processed;
    if (options.on_progress) {
      options.on_progress(window_index + 1, windows.size());
    }
  }

  result.assembled = assembler.finish();
  if (result.frames_decoded == 0) {
    result.blocker = "visual entity tracking decoded no frames";
  }
  return result;
}

}  // namespace svp::vision
