#include "svp/vision/visual_entity_window_fold.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace svp::vision {

VisualEntityWindowFold::VisualEntityWindowFold(const VisualEntityPipelineOptions& options,
                                               const VisualEntityPipelinePlan& plan,
                                               VisualEntityWindowRuntimeStatus runtimes,
                                               FrameCatalog* frame_catalog)
    : options_(options),
      plan_(plan),
      runtimes_(std::move(runtimes)),
      frame_catalog_(frame_catalog),
      assembler_(options.assembly) {
  if (plan_.windows.empty()) {
    throw std::logic_error("a visual entity window fold needs at least one window");
  }
  result_.windows_planned = plan_.windows.size();
  if (!runtimes_.detector_loaded && !runtimes_.detector_blocker.empty()) {
    record_failure("objectness_detector", runtimes_.detector_blocker, 0, plan_.duration_us);
  }
  if (!runtimes_.depth_loaded && !runtimes_.depth_blocker.empty()) {
    record_failure("depth_inference", runtimes_.depth_blocker, 0, plan_.duration_us);
  }
  if (options_.on_progress) options_.on_progress(0, plan_.windows.size());
}

void VisualEntityWindowFold::record_failure(const std::string& component,
                                            const std::string& message,
                                            std::int64_t window_start_us,
                                            std::int64_t window_end_us) {
  const bool already_recorded = std::any_of(
      result_.failures.begin(), result_.failures.end(), [&](const nlohmann::json& failure) {
        return failure.value("component", "") == component &&
               failure.value("message", "") == message &&
               failure.value("window_start_us", std::int64_t{-1}) == window_start_us &&
               failure.value("window_end_us", std::int64_t{-1}) == window_end_us;
      });
  if (already_recorded) return;
  result_.failures.push_back({{"component", component},
                              {"message", message},
                              {"window_start_us", window_start_us},
                              {"window_end_us", window_end_us}});
}

void VisualEntityWindowFold::add_window_provenance(EntityTrackResult& window_result) const {
  window_result.model_refs.insert(window_result.model_refs.end(),
                                  runtimes_.detector_model_refs.begin(),
                                  runtimes_.detector_model_refs.end());
  std::sort(window_result.model_refs.begin(), window_result.model_refs.end());
  window_result.model_refs.erase(
      std::unique(window_result.model_refs.begin(), window_result.model_refs.end()),
      window_result.model_refs.end());
  if (!runtimes_.detector_loaded && !runtimes_.detector_blocker.empty()) {
    window_result.limitations_note +=
        " Objectness detector unavailable: " + runtimes_.detector_blocker + ".";
  }
  if (!runtimes_.depth_loaded && !runtimes_.depth_blocker.empty()) {
    window_result.limitations_note +=
        " Depth support unavailable: " + runtimes_.depth_blocker + ".";
  }
  const VisualEntityDetectorOptions detector = visual_entity_window_detector_options(options_);
  const auto& assembly = options_.assembly;
  const auto& cuts = options_.cut_detection;
  window_result.parameters_json["visual_entity_sampling"] = {
      {"quality", std::string(visual_tracking_quality_name(options_.quality))},
      {"sample_interval_us", plan_.sampling.sample_interval_us},
      {"window_duration_us", plan_.sampling.window_duration_us},
      {"window_overlap_us", plan_.sampling.window_overlap_us}};
  window_result.parameters_json["depth_schedule"] = {
      {"periodic_interval_us", plan_.depth_schedule.periodic_interval_us},
      {"scene_change_threshold", plan_.depth_schedule.scene_change_threshold},
      {"scene_change_burst_frames", plan_.depth_schedule.scene_change_burst_frames}};
  window_result.parameters_json["window_assembly"] = {
      {"minimum_overlap_iou", assembly.minimum_overlap_iou},
      {"maximum_entity_area_ratio", assembly.maximum_entity_area_ratio},
      {"minimum_observation_count", assembly.minimum_observation_count},
      {"minimum_reacquisition_similarity", assembly.minimum_reacquisition_similarity},
      {"minimum_reacquisition_embedding_observations",
       assembly.minimum_reacquisition_embedding_observations},
      {"minimum_supported_fragment_similarity",
       assembly.minimum_supported_fragment_similarity},
      {"minimum_supported_fragment_area_similarity",
       assembly.minimum_supported_fragment_area_similarity},
      {"minimum_supported_fragment_gap_us", assembly.minimum_supported_fragment_gap_us},
      {"maximum_motion_group_gap_us", assembly.maximum_motion_group_gap_us},
      {"minimum_motion_group_endpoint_iou", assembly.minimum_motion_group_endpoint_iou},
      {"handoff_retention_us", assembly.handoff_retention_us},
      {"maximum_identity_evidence_regions", assembly.maximum_identity_evidence_regions},
      {"streaming_enabled", static_cast<bool>(assembly.artifact_sink)},
      {"diagnostic_in_memory", assembly.retain_artifacts_in_memory}};
  window_result.parameters_json["objectness_detector"] = {
      {"model_id", detector.model_id},
      {"model_identity", runtimes_.detector_model_identity},
      {"confidence_threshold", detector.confidence_threshold},
      {"category_evidence_confidence_threshold",
       detector.category_evidence_confidence_threshold},
      {"nms_iou_threshold", detector.nms_iou_threshold},
      {"nms_containment_threshold", detector.nms_containment_threshold},
      {"cross_category_duplicate_iou_threshold",
       detector.cross_category_duplicate_iou_threshold},
      {"minimum_area_ratio", detector.minimum_area_ratio},
      {"maximum_area_ratio", detector.maximum_area_ratio},
      {"maximum_detections", detector.maximum_detections}};
  window_result.parameters_json["cut_detection"] = {
      {"immediate_difference_threshold", cuts.immediate_difference_threshold},
      {"stable_difference_threshold", cuts.stable_difference_threshold},
      {"hard_cut_difference_threshold", cuts.hard_cut_difference_threshold},
      {"sustained_transition_difference_threshold",
       cuts.sustained_transition_difference_threshold},
      {"sustained_transition_observations", cuts.sustained_transition_observations},
      {"extended_stability_lookahead_frames", cuts.extended_stability_lookahead_frames}};
}

void VisualEntityWindowFold::append(std::size_t window_index,
                                    VisualEntityWindowOutcome outcome) {
  if (window_index != next_window_ || window_index >= plan_.windows.size()) {
    throw std::logic_error("visual entity windows must be folded in window order");
  }
  if (outcome.status == VisualEntityWindowStatus::not_started) {
    throw std::logic_error("a window that did not start cannot be folded");
  }
  const auto& window = plan_.windows[window_index];
  if (frame_catalog_ != nullptr) {
    for (std::size_t index = 0; index < outcome.decoded_timestamps_us.size(); ++index) {
      (void)frame_catalog_->register_frame(outcome.decoded_timestamps_us[index],
                                           kVisualEntityTrackingFramePurpose, index == 0);
    }
  }
  result_.frames_attempted += outcome.frames_attempted;
  result_.frames_decoded += outcome.frames_decoded;
  result_.frames_missed += outcome.frames_missed;
  detector_diagnostics_.queries_evaluated += outcome.detector_diagnostics.queries_evaluated;
  detector_diagnostics_.confidence_filtered +=
      outcome.detector_diagnostics.confidence_filtered;
  detector_diagnostics_.area_filtered += outcome.detector_diagnostics.area_filtered;
  detector_diagnostics_.duplicate_filtered += outcome.detector_diagnostics.duplicate_filtered;
  detector_diagnostics_.cap_filtered += outcome.detector_diagnostics.cap_filtered;
  detector_diagnostics_.detections_emitted += outcome.detector_diagnostics.detections_emitted;

  for (const auto& evidence : outcome.cut_evidence) {
    const bool already_recorded = std::any_of(
        result_.cut_evidence.begin(), result_.cut_evidence.end(),
        [&](const auto& existing) { return existing.timestamp_us == evidence.timestamp_us; });
    if (!already_recorded) result_.cut_evidence.push_back(evidence);
  }
  for (const auto& failure : outcome.failures) {
    record_failure(failure.component, failure.message, window.start_us, window.end_us);
  }

  if (outcome.status == VisualEntityWindowStatus::tracked) {
    add_window_provenance(outcome.tracker_result);
    try {
      assembler_.append_window(std::move(outcome.tracker_result), window.timestamps_us,
                               window.start_us, previous_window_end_us_,
                               outcome.cut_timestamps_us);
      previous_window_end_us_ = window.end_us;
      ++result_.windows_succeeded;
    } catch (const std::exception& error) {
      record_failure("visual_entity_assembly", error.what(), window.start_us, window.end_us);
    }
  }
  ++result_.windows_processed;
  ++next_window_;
  if (options_.on_progress) options_.on_progress(window_index + 1, plan_.windows.size());
}

VisualEntityPipelineResult VisualEntityWindowFold::finish() {
  if (next_window_ != plan_.windows.size()) {
    throw std::logic_error("visual entity window fold finished before its last window");
  }
  result_.assembled = assembler_.finish();
  auto& tracker = result_.assembled.tracker_result;
  tracker.parameters_json["objectness_detector"]["diagnostics"] = {
      {"queries_evaluated", detector_diagnostics_.queries_evaluated},
      {"confidence_filtered", detector_diagnostics_.confidence_filtered},
      {"area_filtered", detector_diagnostics_.area_filtered},
      {"duplicate_filtered", detector_diagnostics_.duplicate_filtered},
      {"cap_filtered", detector_diagnostics_.cap_filtered},
      {"detections_emitted", detector_diagnostics_.detections_emitted}};
  tracker.parameters_json["window_failures"] = result_.failures;
  if (result_.windows_succeeded == 0) {
    result_.blocker = "visual entity tracking completed no windows";
    tracker.processing_status = "blocked";
  } else if (!result_.failures.empty()) {
    tracker.processing_status = "partial";
    tracker.limitations_note +=
        " One or more visual entity components or windows failed; see "
        "parameters.window_failures.";
  }
  return std::move(result_);
}

}  // namespace svp::vision
