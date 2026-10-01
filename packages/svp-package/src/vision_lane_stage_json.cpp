#include "svp/package/vision_lane_stages.hpp"

namespace svp::package {

nlohmann::json to_json(const VisionDepthStageResult& result) {
  return {
      {"depth_index_written", result.depth_index_written},
      {"depth_blocks_written", result.depth_blocks_written},
      {"depth_placeholder_written", result.depth_placeholder_written},
      {"depth_generation_run", result.depth_generation_run},
      {"depth_model_available", result.depth_model_available},
      {"depth_model_verified", result.depth_model_verified},
      {"depth_frame_input_available", result.depth_frame_input_available},
      {"depth_generation_detail", result.depth_generation_detail},
      {"processor", result.processor},
  };
}

nlohmann::json to_json(const VisionOcrStageResult& result) {
  return {
      {"ocr_available", result.ocr_available},
      {"ocr_frame_input_available", result.ocr_frame_input_available},
      {"ocr_detection_run", result.ocr_detection_run},
      {"ocr_recognition_run", result.ocr_recognition_run},
      {"text_observation_count", result.text_observation_count},
      {"numeric_value_count", result.numeric_value_count},
      {"ocr_generation_detail", result.ocr_generation_detail},
      {"processors", result.processors},
  };
}

nlohmann::json to_json(const VisionTextEmbeddingStageResult& result) {
  return {
      {"embedding_sets_written", result.embedding_sets_written},
      {"embeddings_index_written", result.embeddings_index_written},
      {"embeddings_blocks_written", result.embeddings_blocks_written},
      {"embedding_generation_run", result.embedding_generation_run},
      {"embedding_model_available", result.embedding_model_available},
      {"embedding_generation_detail", result.embedding_generation_detail},
      {"processor", result.processor},
  };
}

nlohmann::json to_json(const VisionTrackingStageResult& result) {
  return {
      {"masks_index_written", result.masks_index_written},
      {"masks_blocks_written", result.masks_blocks_written},
      {"processor", result.processor},
  };
}

VisionDepthStageResult vision_depth_stage_result_from_json(const nlohmann::json& value) {
  VisionDepthStageResult result;
  result.depth_index_written = value.at("depth_index_written").get<bool>();
  result.depth_blocks_written = value.at("depth_blocks_written").get<bool>();
  result.depth_placeholder_written = value.at("depth_placeholder_written").get<bool>();
  result.depth_generation_run = value.at("depth_generation_run").get<bool>();
  result.depth_model_available = value.at("depth_model_available").get<bool>();
  result.depth_model_verified = value.at("depth_model_verified").get<bool>();
  result.depth_frame_input_available =
      value.at("depth_frame_input_available").get<bool>();
  result.depth_generation_detail = value.at("depth_generation_detail");
  result.processor = value.at("processor");
  return result;
}

VisionOcrStageResult vision_ocr_stage_result_from_json(const nlohmann::json& value) {
  VisionOcrStageResult result;
  result.ocr_available = value.at("ocr_available").get<bool>();
  result.ocr_frame_input_available = value.at("ocr_frame_input_available").get<bool>();
  result.ocr_detection_run = value.at("ocr_detection_run").get<bool>();
  result.ocr_recognition_run = value.at("ocr_recognition_run").get<bool>();
  result.text_observation_count = value.at("text_observation_count").get<std::size_t>();
  result.numeric_value_count = value.at("numeric_value_count").get<std::size_t>();
  result.ocr_generation_detail = value.at("ocr_generation_detail");
  result.processors = value.at("processors").get<std::vector<nlohmann::json>>();
  return result;
}

VisionTextEmbeddingStageResult vision_text_embedding_stage_result_from_json(
    const nlohmann::json& value) {
  VisionTextEmbeddingStageResult result;
  result.embedding_sets_written = value.at("embedding_sets_written").get<bool>();
  result.embeddings_index_written = value.at("embeddings_index_written").get<bool>();
  result.embeddings_blocks_written = value.at("embeddings_blocks_written").get<bool>();
  result.embedding_generation_run = value.at("embedding_generation_run").get<bool>();
  result.embedding_model_available = value.at("embedding_model_available").get<bool>();
  result.embedding_generation_detail = value.at("embedding_generation_detail");
  result.processor = value.at("processor");
  return result;
}

VisionTrackingStageResult vision_tracking_stage_result_from_json(
    const nlohmann::json& value) {
  VisionTrackingStageResult result;
  result.masks_index_written = value.at("masks_index_written").get<bool>();
  result.masks_blocks_written = value.at("masks_blocks_written").get<bool>();
  result.processor = value.at("processor");
  return result;
}

}  // namespace svp::package
