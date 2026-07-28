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
  const auto record_failure = [&](const std::string& component,
                                  const std::string& message,
                                  std::int64_t window_start_us,
                                  std::int64_t window_end_us) {
    const bool already_recorded = std::any_of(
        result.failures.begin(), result.failures.end(),
        [&](const nlohmann::json& failure) {
          return failure.value("component", "") == component &&
              failure.value("message", "") == message &&
              failure.value("window_start_us", std::int64_t{-1}) ==
                  window_start_us &&
              failure.value("window_end_us", std::int64_t{-1}) ==
                  window_end_us;
        });
    if (already_recorded) return;
    result.failures.push_back({
        {"component", component},
        {"message", message},
        {"window_start_us", window_start_us},
        {"window_end_us", window_end_us}});
  };
  if (!detector_runtime.session && !detector_runtime.blocker.empty()) {
    record_failure("objectness_detector", detector_runtime.blocker, 0,
                   duration_us);
  }
  if (!depth_runtime.session && !depth_runtime.blocker.empty()) {
    record_failure("depth_inference", depth_runtime.blocker, 0, duration_us);
  }
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
          } catch (const std::exception& error) {
            record_failure("objectness_detector", error.what(),
                           window.start_us, window.end_us);
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
              record_failure(
                  "depth_inference",
                  "depth output dimensions did not match the canonical raster",
                  window.start_us, window.end_us);
              window_depth.clear();
              depth_frame_ids.clear();
              break;
            }
            window_depth.insert(
                window_depth.end(), frame_depth.begin(), frame_depth.end());
            depth_frame_ids.push_back(frame.frame_id);
          } catch (const std::exception& error) {
            record_failure("depth_inference", error.what(),
                           window.start_us, window.end_us);
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

      EntityTrackResult window_result;
      try {
        window_result = run_visual_entity_tracker(
            decoded.frames,
            window_depth,
            depth_frame_ids,
            cut_boundaries,
            model_cache_root,
            tracker_options);
      } catch (const std::exception& error) {
        record_failure("visual_entity_tracker", error.what(),
                       window.start_us, window.end_us);
        ++result.windows_processed;
        if (options.on_progress) {
          options.on_progress(window_index + 1, windows.size());
        }
        continue;
      }
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
      window_result.parameters_json["depth_schedule"] = {
          {"periodic_interval_us",
           options.depth_schedule.periodic_interval_us},
          {"scene_change_threshold",
           options.depth_schedule.scene_change_threshold},
          {"scene_change_burst_frames",
           options.depth_schedule.scene_change_burst_frames}};
      window_result.parameters_json["window_assembly"] = {
          {"minimum_overlap_iou", options.assembly.minimum_overlap_iou},
          {"maximum_entity_area_ratio",
           options.assembly.maximum_entity_area_ratio},
          {"minimum_observation_count",
           options.assembly.minimum_observation_count},
          {"minimum_reacquisition_similarity",
           options.assembly.minimum_reacquisition_similarity},
          {"minimum_reacquisition_embedding_observations",
           options.assembly.minimum_reacquisition_embedding_observations},
          {"minimum_supported_fragment_similarity",
           options.assembly.minimum_supported_fragment_similarity},
          {"minimum_supported_fragment_area_similarity",
           options.assembly.minimum_supported_fragment_area_similarity},
          {"minimum_supported_fragment_gap_us",
           options.assembly.minimum_supported_fragment_gap_us},
          {"maximum_motion_group_gap_us",
           options.assembly.maximum_motion_group_gap_us},
          {"minimum_motion_group_endpoint_iou",
           options.assembly.minimum_motion_group_endpoint_iou},
          {"handoff_retention_us", options.assembly.handoff_retention_us},
          {"maximum_identity_evidence_regions",
           options.assembly.maximum_identity_evidence_regions},
          {"streaming_enabled", static_cast<bool>(options.assembly.artifact_sink)},
          {"diagnostic_in_memory",
           options.assembly.retain_artifacts_in_memory}};
      window_result.parameters_json["objectness_detector"] = {
          {"model_id", options.detector.model_id},
          {"model_identity", detector_runtime.model_identity},
          {"confidence_threshold", options.detector.confidence_threshold},
          {"category_evidence_confidence_threshold",
           options.detector.category_evidence_confidence_threshold},
          {"nms_iou_threshold", options.detector.nms_iou_threshold},
          {"nms_containment_threshold",
           options.detector.nms_containment_threshold},
          {"cross_category_duplicate_iou_threshold",
           options.detector.cross_category_duplicate_iou_threshold},
          {"minimum_area_ratio", options.detector.minimum_area_ratio},
          {"maximum_area_ratio", options.detector.maximum_area_ratio},
          {"maximum_detections", options.detector.maximum_detections}};
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
           options.cut_detection.sustained_transition_observations},
          {"extended_stability_lookahead_frames",
           options.cut_detection.extended_stability_lookahead_frames}};
      try {
        assembler.append_window(
            std::move(window_result),
            window.timestamps_us,
            window.start_us,
            previous_window_end_us,
            cut_timestamps_us);
        previous_window_end_us = window.end_us;
        ++result.windows_succeeded;
      } catch (const std::exception& error) {
        record_failure("visual_entity_assembly", error.what(),
                       window.start_us, window.end_us);
      }
    } else {
      record_failure(
          "frame_decode",
          decoded.decoding_succeeded
              ? "fewer than two visual entity frames were decoded"
              : decoded.skipped_reason,
          window.start_us, window.end_us);
    }

    ++result.windows_processed;
    if (options.on_progress) {
      options.on_progress(window_index + 1, windows.size());
    }
  }

  result.assembled = assembler.finish();
  result.assembled.tracker_result.parameters_json["objectness_detector"]
      ["diagnostics"] = {
          {"queries_evaluated",
           detector_runtime.diagnostics.queries_evaluated},
          {"confidence_filtered",
           detector_runtime.diagnostics.confidence_filtered},
          {"area_filtered", detector_runtime.diagnostics.area_filtered},
          {"duplicate_filtered",
           detector_runtime.diagnostics.duplicate_filtered},
          {"cap_filtered", detector_runtime.diagnostics.cap_filtered},
          {"detections_emitted",
           detector_runtime.diagnostics.detections_emitted}};
  result.assembled.tracker_result.parameters_json["window_failures"] =
      result.failures;
  if (result.windows_succeeded == 0) {
    result.blocker = "visual entity tracking completed no windows";
    result.assembled.tracker_result.processing_status = "blocked";
  } else if (!result.failures.empty()) {
    result.assembled.tracker_result.processing_status = "partial";
    result.assembled.tracker_result.limitations_note +=
        " One or more visual entity components or windows failed; see "
        "parameters.window_failures.";
  }
  return result;
}

}  // namespace svp::vision
