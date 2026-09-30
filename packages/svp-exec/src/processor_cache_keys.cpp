#include "svp/exec/processor_cache_keys.hpp"

#include "svp/exec/cache_key.hpp"
#include "svp/exec/exec_error.hpp"

#include <string_view>

namespace svp::exec {
namespace {

const std::string& non_empty(const std::string& value, std::string_view name) {
  if (value.empty()) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "cache key field `" + std::string(name) +
                        "` must not be empty");
  }
  return value;
}

void append_common_prefix(nlohmann::json& fields,
                          const std::string& svp_core_schema_version,
                          const std::string& processor_id,
                          const std::string& processor_version,
                          const CacheKeyModelIdentity& model) {
  fields.push_back(non_empty(svp_core_schema_version, "svp_core_schema_version"));
  fields.push_back(non_empty(processor_id, "processor_id"));
  fields.push_back(non_empty(processor_version, "processor_version"));
  fields.push_back(non_empty(model.model_id, "model_id"));
  fields.push_back(non_empty(model.model_bundle_id, "model_bundle_id"));
  fields.push_back(blake3_hex(model.bundle_blake3));
  fields.push_back(blake3_hex(model.model_blake3));
  fields.push_back(non_empty(model.model_runtime, "model_runtime"));
  fields.push_back(non_empty(model.execution_provider, "execution_provider"));
}

nlohmann::json time_range_field(const CacheKeyTimeRangeUs& range) {
  if (range.start_us < 0 || range.end_us < range.start_us) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "cache key time_range must satisfy 0 <= start_us <= end_us");
  }
  return nlohmann::json::array({range.start_us, range.end_us});
}

}  // namespace

nlohmann::json visual_cache_key_fields(const VisualCacheKeyInputs& inputs) {
  nlohmann::json fields = nlohmann::json::array();
  append_common_prefix(fields, inputs.svp_core_schema_version,
                       inputs.processor_id, inputs.processor_version,
                       inputs.model);
  fields.push_back(
      non_empty(inputs.normalized_input_format, "normalized_input_format"));
  fields.push_back(time_range_field(inputs.time_range));
  fields.push_back(inputs.frame_indices);
  fields.push_back(blake3_hex(inputs.decoded_pixel_blake3));
  fields.push_back(blake3_hex(inputs.processor_parameters_blake3));
  return fields;
}

nlohmann::json audio_cache_key_fields(const AudioCacheKeyInputs& inputs) {
  nlohmann::json fields = nlohmann::json::array();
  append_common_prefix(fields, inputs.svp_core_schema_version,
                       inputs.processor_id, inputs.processor_version,
                       inputs.model);
  fields.push_back(inputs.sample_rate);
  fields.push_back(non_empty(inputs.channel_layout, "channel_layout"));
  fields.push_back(non_empty(inputs.sample_format, "sample_format"));
  fields.push_back(blake3_hex(inputs.audio_chunk_blake3));
  fields.push_back(blake3_hex(inputs.processor_parameters_blake3));
  return fields;
}

Blake3Digest visual_cache_key(const VisualCacheKeyInputs& inputs) {
  return compute_cache_key(visual_cache_key_fields(inputs));
}

Blake3Digest audio_cache_key(const AudioCacheKeyInputs& inputs) {
  return compute_cache_key(audio_cache_key_fields(inputs));
}

}  // namespace svp::exec
