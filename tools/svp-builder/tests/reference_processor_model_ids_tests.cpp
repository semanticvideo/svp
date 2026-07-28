#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/diarization_boundary.hpp"
#include "svp/models/reference_model_set.hpp"
#include "svp/vision/depth_generation.hpp"
#include "svp/vision/embedding_generation.hpp"
#include "svp/vision/pp_ocr.hpp"
#include "svp/vision/visual_entity_detector.hpp"
#include "svp/vision/visual_entity_pipeline.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: svp-builder-reference-model-ids-tests "
                 "<reference-model-set.json>\n";
    return 2;
  }

  const auto model_set =
      svp::models::load_reference_model_set(std::filesystem::path(argv[1]));
  std::set<std::string> registered_ids;
  for (const auto& model : model_set.models) {
    if (!registered_ids.insert(model.model_id).second) {
      std::cerr << "duplicate reference model ID: " << model.model_id << '\n';
      return 1;
    }
  }

  const auto asr_plan = svp::audio::build_asr_chunk_plan(1'000'000);
  if (asr_plan.chunks.empty()) {
    std::cerr << "ASR runtime did not produce a default chunk plan\n";
    return 1;
  }

  const svp::vision::DepthGenerationOptions depth;
  const svp::vision::EmbeddingGenerationOptions embeddings;
  const svp::vision::VisualEntityDetectorOptions detector;
  const svp::vision::VisualEntityPipelineOptions entity_pipeline;
  const svp::vision::VisualEntityTrackerOptions entity_tracker;
  const svp::vision::PpOcrOptions ocr;

  const std::vector<std::pair<std::string, std::string>> runtime_consumers = {
      {"audio.asr_chunk_planner", asr_plan.chunks.front().model_id},
      {"audio.asr_boundary", svp::audio::AsrExecutionBoundary{}.model_id},
      {"audio.diarization_boundary",
       svp::audio::DiarizationExecutionBoundary{}.model_id},
      {"vision.depth_generation", depth.model_id},
      {"vision.text_embeddings", embeddings.text_model_id},
      {"vision.image_embeddings", embeddings.vision_model_id},
      {"vision.entity_detector", detector.model_id},
      {"vision.entity_embeddings", entity_pipeline.embedding_model_id},
      {"vision.tracker_embeddings", entity_tracker.embedding_model_id},
      {"vision.ocr_detector", ocr.detector_model_id},
      {"vision.ocr_recognizer", ocr.recognizer_model_id},
  };

  std::set<std::string> runtime_ids;
  for (const auto& [consumer, model_id] : runtime_consumers) {
    runtime_ids.insert(model_id);
    if (!registered_ids.contains(model_id)) {
      std::cerr << consumer << " uses unregistered model ID: " << model_id
                << '\n';
      return 1;
    }
  }

  if (runtime_ids != registered_ids) {
    for (const auto& model_id : registered_ids) {
      if (!runtime_ids.contains(model_id)) {
        std::cerr << "reference registry model has no production consumer: "
                  << model_id << '\n';
      }
    }
    return 1;
  }

  if (ocr.manifest_filename != "model.svpmodel.json") {
    std::cerr << "PP-OCR runtime must use canonical model.svpmodel.json; got "
              << ocr.manifest_filename << '\n';
    return 1;
  }

  return 0;
}
