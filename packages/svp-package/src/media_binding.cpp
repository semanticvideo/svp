#include "svp/package/media_binding.hpp"

#include <blake3.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace svp::package {

namespace {

std::string blake3_hex_for_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = file.gcount();
    if (count > 0) {
      blake3_hasher_update(&hasher, buffer.data(), static_cast<size_t>(count));
    }
  }

  std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());

  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return "blake3:" + stream.str();
}

std::int64_t file_size_bytes(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return 0;
  }
  return static_cast<std::int64_t>(size);
}

}  // namespace

std::string to_string(Blake3State state) noexcept {
  switch (state) {
    case Blake3State::present:
      return "present";
    case Blake3State::pending:
      return "pending";
    case Blake3State::unavailable:
      return "unavailable";
  }
  return "pending";
}

nlohmann::json to_json(const StreamMetadata& metadata) {
  nlohmann::json obj = nlohmann::json::object();
  if (!metadata.codec_name.empty()) {
    obj["codec_name"] = metadata.codec_name;
  }
  if (!metadata.codec_long_name.empty()) {
    obj["codec_long_name"] = metadata.codec_long_name;
  }
  if (!metadata.profile.empty()) {
    obj["profile"] = metadata.profile;
  }
  if (metadata.bit_rate > 0) {
    obj["bit_rate"] = metadata.bit_rate;
  }
  if (metadata.width.has_value()) {
    obj["width"] = metadata.width.value();
  }
  if (metadata.height.has_value()) {
    obj["height"] = metadata.height.value();
  }
  if (metadata.frame_rate.has_value()) {
    obj["frame_rate"] = metadata.frame_rate.value();
  }
  if (metadata.sample_rate.has_value()) {
    obj["sample_rate"] = metadata.sample_rate.value();
  }
  if (metadata.channels.has_value()) {
    obj["channels"] = metadata.channels.value();
  }
  if (metadata.bits_per_sample.has_value()) {
    obj["bits_per_sample"] = metadata.bits_per_sample.value();
  }
  if (metadata.duration_us.has_value()) {
    obj["duration_us"] = metadata.duration_us.value();
  }
  return obj;
}

nlohmann::json to_json(const ChunkProof& proof) {
  return {
    {"chunk_size_bytes", proof.chunk_size_bytes},
    {"chunk_count", proof.chunk_count},
    {"last_chunk_hash", proof.last_chunk_hash},
    {"last_chunk_size", proof.last_chunk_size},
  };
}

nlohmann::json to_json(const MediaIdentity& identity) {
  nlohmann::json obj = nlohmann::json::object();
  obj["media_id"] = identity.media_id;
  obj["size_bytes"] = identity.size_bytes;
  obj["duration_us"] = identity.duration_us;
  obj["container_format"] = identity.container_format;

  nlohmann::json streams = nlohmann::json::array();
  for (const auto& stream : identity.streams) {
    streams.push_back(to_json(stream));
  }
  obj["streams"] = streams;

  if (identity.chunk_proof.has_value()) {
    obj["chunk_proof"] = to_json(identity.chunk_proof.value());
  }

  nlohmann::json blake3 = nlohmann::json::object();
  blake3["state"] = to_string(identity.blake3_state);
  if (!identity.blake3_hash.empty()) {
    blake3["hash"] = identity.blake3_hash;
  }
  if (!identity.blake3_state_reason.empty()) {
    blake3["reason"] = identity.blake3_state_reason;
  }
  obj["full_file_blake3"] = blake3;

  if (!identity.original_filename_hint.empty()) {
    obj["original_filename_hint"] = identity.original_filename_hint;
  }

  return obj;
}

nlohmann::json to_json(const MediaBinding& binding) {
  return {
    {"binding_id", binding.binding_id},
    {"binding_contract", binding.binding_contract},
    {"verification_state", binding.verification_state},
    {"identity", to_json(binding.identity)},
  };
}

nlohmann::json to_json(const MediaBindingDocument& doc) {
  return {
    {"schema", doc.schema},
    {"primary_source", to_json(doc.primary_source)},
  };
}

bool write_media_binding(
    const std::filesystem::path& output_path,
    const MediaBindingDocument& doc) {
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream out(output_path);
  if (!out) {
    return false;
  }
  out << to_json(doc).dump(2) << "\n";
  return true;
}

}  // namespace svp::package
