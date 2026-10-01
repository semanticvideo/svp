#include "svp/package/media_binding.hpp"

#include <stdexcept>

namespace svp::package {
namespace {

std::string string_or_empty(const nlohmann::json& object, const char* key) {
  const auto found = object.find(key);
  return found == object.end() ? std::string{} : found->get<std::string>();
}

template <typename T>
std::optional<T> optional_value(const nlohmann::json& object, const char* key) {
  const auto found = object.find(key);
  if (found == object.end()) {
    return std::nullopt;
  }
  return found->get<T>();
}

Blake3State blake3_state_from_string(const std::string& value) {
  if (value == "present") return Blake3State::present;
  if (value == "pending") return Blake3State::pending;
  if (value == "unavailable") return Blake3State::unavailable;
  throw std::invalid_argument("unknown media binding blake3 state: " + value);
}

StreamMetadata stream_from_json(const nlohmann::json& value) {
  StreamMetadata stream;
  stream.codec_name = string_or_empty(value, "codec_name");
  stream.codec_long_name = string_or_empty(value, "codec_long_name");
  stream.profile = string_or_empty(value, "profile");
  stream.bit_rate = optional_value<std::int64_t>(value, "bit_rate").value_or(0);
  stream.width = optional_value<std::int64_t>(value, "width");
  stream.height = optional_value<std::int64_t>(value, "height");
  stream.frame_rate = optional_value<double>(value, "frame_rate");
  stream.sample_rate = optional_value<std::int64_t>(value, "sample_rate");
  stream.channels = optional_value<std::int64_t>(value, "channels");
  stream.bits_per_sample = optional_value<std::int64_t>(value, "bits_per_sample");
  stream.duration_us = optional_value<std::int64_t>(value, "duration_us");
  return stream;
}

MediaIdentity identity_from_json(const nlohmann::json& value) {
  MediaIdentity identity;
  const nlohmann::json& blake3 = value.at("full_file_blake3");
  identity.blake3_state = blake3_state_from_string(blake3.at("state").get<std::string>());
  identity.blake3_hash = string_or_empty(blake3, "value");
  identity.blake3_state_reason = string_or_empty(blake3, "reason");
  // to_json writes a zero chunk record without last_chunk_* fields when no
  // proof was computed.
  const nlohmann::json& chunks = value.at("chunk_hashes");
  if (chunks.contains("last_chunk_hash")) {
    identity.chunk_proof = ChunkProof{
        .chunk_size_bytes = chunks.at("chunk_size_bytes").get<std::int64_t>(),
        .chunk_count = chunks.at("chunk_count").get<std::int64_t>(),
        .last_chunk_hash = chunks.at("last_chunk_hash").get<std::string>(),
        .last_chunk_size = chunks.at("last_chunk_size").get<std::int64_t>(),
    };
  }
  return identity;
}

LocationHints location_hints_from_json(const nlohmann::json& value) {
  return LocationHints{
      .original_filename = string_or_empty(value, "original_filename"),
      .relative_path = string_or_empty(value, "relative_path"),
      .original_absolute_path = string_or_empty(value, "original_absolute_path"),
      .volume_hint = string_or_empty(value, "volume_hint"),
      .last_seen_utc = string_or_empty(value, "last_seen_utc"),
  };
}

MediaBinding binding_from_json(const nlohmann::json& value) {
  MediaBinding binding;
  binding.binding_id = value.at("binding_id").get<std::string>();
  binding.media_role = value.at("media_role").get<std::string>();
  binding.media_id = value.at("media_id").get<std::string>();
  binding.binding_contract = value.at("binding_contract").get<std::string>();
  binding.verification_state = value.at("verification_state").get<std::string>();
  binding.duration_us = value.at("duration_us").get<std::int64_t>();
  binding.size_bytes = value.at("size_bytes").get<std::int64_t>();
  binding.container_format = value.at("container_format").get<std::string>();
  for (const nlohmann::json& stream : value.at("streams")) {
    binding.streams.push_back(stream_from_json(stream));
  }
  binding.identity = identity_from_json(value.at("identity"));
  binding.location_hints = location_hints_from_json(value.at("location_hints"));
  return binding;
}

}  // namespace

MediaBindingDocument media_binding_document_from_json(const nlohmann::json& value) {
  MediaBindingDocument document;
  document.schema = value.at("schema").get<std::string>();
  document.primary_binding_id = value.at("primary_binding_id").get<std::string>();
  for (const nlohmann::json& binding : value.at("bindings")) {
    document.bindings.push_back(binding_from_json(binding));
  }
  return document;
}

}  // namespace svp::package
