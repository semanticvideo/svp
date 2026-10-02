#include "svp/vision/tasks/embed_keyframe_batch_parameters.hpp"

#include "onnx_model_parameters_json.hpp"
#include "parameter_fields.hpp"

#include <stdexcept>

namespace svp::vision::tasks {
namespace {

using detail::Json;

const detail::ParameterFields& fields() {
  static const detail::ParameterFields kFields(kEmbedKeyframeBatchTaskType);
  return kFields;
}

Json keyframe_to_json(const OrderedKeyframeItem& keyframe) {
  return Json{{"height", keyframe.item.height},   {"ordinal", keyframe.ordinal},
              {"pts_us", keyframe.item.pts_us},   {"shot_id", keyframe.item.shot_id},
              {"width", keyframe.item.width}};
}

}  // namespace

std::uint64_t embed_keyframe_parameter_bytes(const OrderedKeyframeItem& item) {
  return keyframe_to_json(item).dump().size();
}

Json embed_keyframe_batch_parameters_to_json(const EmbedKeyframeBatchParameters& parameters) {
  Json keyframes = Json::array();
  for (const OrderedKeyframeItem& keyframe : parameters.keyframes) {
    keyframes.push_back(keyframe_to_json(keyframe));
  }
  Json value{{"decode", {{"ffmpeg_build", parameters.ffmpeg_build}}},
             {"embedding_dim", parameters.embedding_dim},
             {"keyframes", std::move(keyframes)}};
  detail::add_onnx_model_fields(value, parameters.model);
  (void)embed_keyframe_batch_parameters_from_json(value);
  return value;
}

EmbedKeyframeBatchParameters embed_keyframe_batch_parameters_from_json(const Json& value) {
  fields().require_fields(value,
                          {"decode", "embedding_dim", "execution_provider", "keyframes",
                           "model_id", "threads"},
                          "parameters");
  EmbedKeyframeBatchParameters parameters;
  parameters.embedding_dim =
      fields().integer_at<std::uint32_t>(value, "embedding_dim", "parameters", 1);
  parameters.model = detail::onnx_model_fields_from_json(fields(), value);
  const Json& decode = value.at("decode");
  fields().require_fields(decode, {"ffmpeg_build"}, "decode");
  parameters.ffmpeg_build = fields().ffmpeg_build_at(decode, "decode");
  const Json& keyframes = fields().array_at(value, "keyframes", "parameters");
  if (keyframes.empty()) fields().reject("keyframes must not be empty");
  for (std::size_t index = 0; index < keyframes.size(); ++index) {
    const std::string where = "keyframes[" + std::to_string(index) + "]";
    const Json& keyframe = keyframes[index];
    fields().require_fields(keyframe, {"height", "ordinal", "pts_us", "shot_id", "width"}, where);
    OrderedKeyframeItem item{
        .ordinal = fields().integer_at<std::uint64_t>(keyframe, "ordinal", where, 0),
        .item = {.shot_id = fields().string_at(keyframe, "shot_id", where),
                 .pts_us = fields().integer_at<std::int64_t>(keyframe, "pts_us", where, 0),
                 .width = fields().integer_at<int>(keyframe, "width", where, 1),
                 .height = fields().integer_at<int>(keyframe, "height", where, 1)}};
    if (!parameters.keyframes.empty() && item.ordinal <= parameters.keyframes.back().ordinal) {
      fields().reject(where + " is not strictly after the keyframe before it");
    }
    parameters.keyframes.push_back(std::move(item));
  }
  return parameters;
}

std::optional<std::string> validate_embed_keyframe_batch_parameters(const Json& value) {
  try {
    (void)embed_keyframe_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string(kEmbedKeyframeBatchTaskType) + " parameters: " + error.what();
  }
}

}  // namespace svp::vision::tasks
