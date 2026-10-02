#include "svp/vision/tasks/ocr_frame_batch_parameters.hpp"

#include "parameter_fields.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/tasks/pp_ocr_parameters.hpp"

#include <stdexcept>

namespace svp::vision::tasks {
namespace {

using detail::Json;

const detail::ParameterFields& fields() {
  static const detail::ParameterFields kFields(kOcrFrameBatchTaskType);
  return kFields;
}

std::vector<OcrSample> samples_from_json(const Json& value) {
  const std::string where = "samples";
  fields().require_fields(value, {"ordinals", "timestamps_us"}, where);
  const Json& ordinals = value.at("ordinals");
  const Json& timestamps = value.at("timestamps_us");
  if (!ordinals.is_array() || !timestamps.is_array()) {
    fields().reject("samples.ordinals and samples.timestamps_us must be arrays");
  }
  if (ordinals.empty()) fields().reject("samples must not be empty");
  if (ordinals.size() != timestamps.size()) {
    fields().reject("samples.ordinals and samples.timestamps_us differ in length");
  }
  std::vector<OcrSample> samples;
  samples.reserve(ordinals.size());
  for (std::size_t index = 0; index < ordinals.size(); ++index) {
    const Json pair{{"ordinal", ordinals[index]}, {"timestamp_us", timestamps[index]}};
    const std::string item = where + "[" + std::to_string(index) + "]";
    OcrSample sample{
        .ordinal = fields().integer_at<std::uint64_t>(pair, "ordinal", item, 0),
        .timestamp_us = fields().integer_at<std::int64_t>(pair, "timestamp_us", item, 0),
    };
    if (!samples.empty() && (sample.ordinal <= samples.back().ordinal ||
                             sample.timestamp_us <= samples.back().timestamp_us)) {
      fields().reject(item + " is not strictly after the sample before it");
    }
    samples.push_back(sample);
  }
  return samples;
}

}  // namespace

Json ocr_frame_batch_parameters_to_json(const OcrFrameBatchParameters& parameters) {
  Json ordinals = Json::array();
  Json timestamps = Json::array();
  for (const OcrSample& sample : parameters.samples) {
    ordinals.push_back(sample.ordinal);
    timestamps.push_back(sample.timestamp_us);
  }
  Json value = pp_ocr_parameter_fields(parameters.pp_ocr);
  value["decode"] = {{"ffmpeg_build", parameters.ffmpeg_build},
                     {"frame_height", parameters.frame_height},
                     {"frame_width", parameters.frame_width}};
  value["samples"] = {{"ordinals", std::move(ordinals)},
                      {"timestamps_us", std::move(timestamps)}};
  // One set of rules for both directions.
  (void)ocr_frame_batch_parameters_from_json(value);
  return value;
}

OcrFrameBatchParameters ocr_frame_batch_parameters_from_json(const Json& value) {
  fields().require_fields(value,
                          {"decode", "detector", "execution_provider", "recognizer", "samples"},
                          "parameters");
  OcrFrameBatchParameters parameters;
  parameters.samples = samples_from_json(value.at("samples"));

  const Json& decode = value.at("decode");
  fields().require_fields(decode, {"ffmpeg_build", "frame_height", "frame_width"}, "decode");
  parameters.ffmpeg_build = fields().string_at(decode, "ffmpeg_build", "decode");
  if (!svp::exec::parse_blake3_prefixed(parameters.ffmpeg_build)) {
    fields().reject("decode.ffmpeg_build must be b3:<64 hex>");
  }
  parameters.frame_width =
      fields().integer_at<int>(decode, "frame_width", "decode", 1, kOcrMaxFrameDimension);
  parameters.frame_height =
      fields().integer_at<int>(decode, "frame_height", "decode", 1, kOcrMaxFrameDimension);

  parameters.pp_ocr = pp_ocr_options_from_parameters(value, kOcrFrameBatchTaskType);
  return parameters;
}

std::optional<std::string> validate_ocr_frame_batch_parameters(const Json& value) {
  try {
    (void)ocr_frame_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string("ocr.frame_batch parameters: ") + error.what();
  }
}

}  // namespace svp::vision::tasks
