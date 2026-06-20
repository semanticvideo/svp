#include "svp/vision/observation_pipeline_plan.hpp"

#include <utility>

namespace svp::vision {
namespace {

std::string task_family_to_string(const ObservationTaskFamily family) {
  switch (family) {
    case ObservationTaskFamily::ocr:
      return "ocr";
    case ObservationTaskFamily::color:
      return "color";
    case ObservationTaskFamily::provenance:
      return "provenance";
  }
  return "unknown";
}

ObservationPipelineTask task(std::string task_id,
                             ObservationTaskFamily family,
                             std::string processor_boundary,
                             std::vector<std::string> depends_on,
                             std::vector<std::string> consumes,
                             std::vector<std::string> emits,
                             std::string provenance_hook,
                             bool model_backed,
                             bool deterministic_classical) {
  return ObservationPipelineTask{
      std::move(task_id),
      family,
      std::move(processor_boundary),
      std::move(depends_on),
      std::move(consumes),
      std::move(emits),
      std::move(provenance_hook),
      model_backed,
      deterministic_classical,
  };
}

nlohmann::json task_to_json(const ObservationPipelineTask& task) {
  return {
      {"task_id", task.task_id},
      {"family", task_family_to_string(task.family)},
      {"processor_boundary", task.processor_boundary},
      {"depends_on", task.depends_on},
      {"consumes", task.consumes},
      {"emits", task.emits},
      {"provenance_hook", task.provenance_hook},
      {"model_backed", task.model_backed},
      {"deterministic_classical", task.deterministic_classical},
      {"execution_state", "planned_only"},
  };
}

}  // namespace

VisionObservationPipelinePlan build_vision_observation_pipeline_plan(
    const media::MediaIngestPlan& media_plan) {
  std::vector<ObservationPipelineTask> tasks;

  tasks.push_back(task("task.vision.color_space_normalization",
                       ObservationTaskFamily::color,
                       "color_space_normalization",
                       {},
                       {"canonical_analysis_raster_frames"},
                       {"normalized_color_frames"},
                       "processor_color_normalizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.color_bucket_quantization",
                       ObservationTaskFamily::color,
                       "color_bucket_quantization",
                       {"task.vision.color_space_normalization"},
                       {"normalized_color_frames", "spec/registries/color-buckets.json",
                        "spec/registries/color-spaces.json"},
                       {"registry_backed_bucket_samples"},
                       "processor_color_quantizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.shot_color_summary",
                       ObservationTaskFamily::color,
                       "shot_color_summary",
                       {"task.vision.color_bucket_quantization"},
                       {"shots.jsonl", "registry_backed_bucket_samples"},
                       {"colors/color_observations.jsonl"},
                       "processor_color_quantizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.scene_color_summary",
                       ObservationTaskFamily::color,
                       "scene_color_summary",
                       {"task.vision.shot_color_summary"},
                       {"scenes.jsonl", "colors/color_observations.jsonl"},
                       {"colors/color_observations.jsonl", "colors/color_summary.json"},
                       "processor_color_quantizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.frame_color_summary",
                       ObservationTaskFamily::color,
                       "frame_color_summary",
                       {"task.vision.color_bucket_quantization"},
                       {"canonical_frame_index", "registry_backed_bucket_samples"},
                       {"colors/color_observations.jsonl"},
                       "processor_color_quantizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.region_color_summary",
                       ObservationTaskFamily::color,
                       "region_color_summary",
                       {"task.vision.color_bucket_quantization"},
                       {"regions.jsonl", "region_masks_or_crops"},
                       {"colors/color_observations.jsonl"},
                       "processor_color_quantizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.entity_color_summary",
                       ObservationTaskFamily::color,
                       "entity_color_summary",
                       {"task.vision.region_color_summary"},
                       {"entities.jsonl", "regions.jsonl", "colors/color_observations.jsonl"},
                       {"colors/color_observations.jsonl"},
                       "processor_color_quantizer",
                       false,
                       true));
  tasks.push_back(task("task.vision.ocr_text_detection",
                       ObservationTaskFamily::ocr,
                       "ocr_text_detection",
                       {},
                       {"canonical_analysis_raster_frames", "shots.jsonl", "scenes.jsonl",
                        "regions.jsonl", "entities.jsonl"},
                       {"text/text_regions.jsonl", "text/text_absence.json"},
                       "processor_ocr_detector",
                       true,
                       false));
  tasks.push_back(task("task.vision.ocr_text_recognition",
                       ObservationTaskFamily::ocr,
                       "ocr_text_recognition",
                       {"task.vision.ocr_text_detection"},
                       {"text/text_regions.jsonl", "canonical_analysis_raster_frames"},
                       {"text/text_observations.jsonl"},
                       "processor_ocr_recognizer",
                       true,
                       false));
  tasks.push_back(task("task.vision.ocr_layout_classification",
                       ObservationTaskFamily::ocr,
                       "ocr_layout_classification",
                       {"task.vision.ocr_text_recognition"},
                       {"text/text_regions.jsonl", "text/text_observations.jsonl"},
                       {"text/text_observations.jsonl"},
                       "processor_ocr_layout_classifier",
                       true,
                       false));
  tasks.push_back(task("task.vision.ocr_numeric_extraction",
                       ObservationTaskFamily::ocr,
                       "ocr_numeric_extraction",
                       {"task.vision.ocr_text_recognition",
                        "task.vision.ocr_layout_classification"},
                       {"text/text_observations.jsonl"},
                       {"text/numeric_values.jsonl"},
                       "processor_numeric_parser",
                       false,
                       true));
  tasks.push_back(task("task.vision.ocr_text_region_color_sampling",
                       ObservationTaskFamily::color,
                       "ocr_text_region_color_sampling",
                       {"task.vision.ocr_text_detection",
                        "task.vision.color_bucket_quantization"},
                       {"text/text_regions.jsonl", "normalized_color_frames"},
                       {"colors/color_observations.jsonl", "text/text_regions.jsonl"},
                       "processor_text_region_color_sampler",
                       false,
                       true));
  tasks.push_back(task("task.vision.text_region_color_summary",
                       ObservationTaskFamily::color,
                       "text_region_color_summary",
                       {"task.vision.ocr_text_region_color_sampling"},
                       {"text/text_regions.jsonl", "registry_backed_bucket_samples"},
                       {"colors/color_observations.jsonl"},
                       "processor_text_region_color_sampler",
                       false,
                       true));
  tasks.push_back(task("task.vision.text_region_foreground_background_color",
                       ObservationTaskFamily::color,
                       "text_region_foreground_background_color",
                       {"task.vision.text_region_color_summary"},
                       {"text/text_regions.jsonl", "colors/color_observations.jsonl"},
                       {"colors/color_observations.jsonl", "text/text_regions.jsonl"},
                       "processor_text_region_color_sampler",
                       false,
                       true));
  tasks.push_back(task("task.vision.ocr_color_provenance_hooks",
                       ObservationTaskFamily::provenance,
                       "ocr_color_provenance_hooks",
                       {"task.vision.scene_color_summary",
                        "task.vision.ocr_numeric_extraction",
                        "task.vision.text_region_foreground_background_color"},
                       {"task_records", "processor_parameters", "model_bundle_records"},
                       {"provenance/processors.jsonl", "provenance/model_hashes.jsonl"},
                       "processor_provenance_writer",
                       false,
                       true));

  return VisionObservationPipelinePlan{
      "svp-vision-observation-pipeline-plan-v1",
      media_plan.source_path.string(),
      "canonical_analysis_raster",
      std::move(tasks),
      {"real_ocr_or_model_inference",
       "real_video_frame_decoding",
       "final_package_writer",
       "validator_semantic_changes",
       "camera_motion_or_zoom_metrics"},
  };
}

nlohmann::json vision_observation_pipeline_plan_to_json(
    const VisionObservationPipelinePlan& plan) {
  nlohmann::json tasks = nlohmann::json::array();
  for (const ObservationPipelineTask& pipeline_task : plan.tasks) {
    tasks.push_back(task_to_json(pipeline_task));
  }

  return {
      {"schema_version", plan.schema_version},
      {"source", {{"path", plan.source_path}}},
      {"canonical_raster_basis", plan.canonical_raster_basis},
      {"tasks", tasks},
      {"explicit_non_goals", plan.explicit_non_goals},
      {"observations_generated", false},
      {"model_execution_performed", false},
      {"valid_svp_package_written", false},
  };
}

}  // namespace svp::vision
