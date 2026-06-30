#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace svp::package {

inline constexpr std::string_view kSvpiMimetype = "application/vnd.svp.interlace+zip";
inline constexpr std::string_view kSvpiBindingContract = "svpi.media_identity.v0.1";
inline constexpr std::string_view kSvpiVersion = "0.1";
inline constexpr std::string_view kSvpiFormat = "svpi";

enum class Blake3State {
  present,
  pending,
  unavailable,
};

[[nodiscard]] std::string to_string(Blake3State state) noexcept;

struct StreamMetadata {
  std::string codec_name;
  std::string codec_long_name;
  std::string profile;
  std::int64_t bit_rate = 0;
  std::optional<std::int64_t> width;
  std::optional<std::int64_t> height;
  std::optional<double> frame_rate;
  std::optional<std::int64_t> sample_rate;
  std::optional<std::int64_t> channels;
  std::optional<std::int64_t> bits_per_sample;
  std::optional<std::int64_t> duration_us;
};

struct ChunkProof {
  std::int64_t chunk_size_bytes = 0;
  std::int64_t chunk_count = 0;
  std::string last_chunk_hash;
  std::int64_t last_chunk_size = 0;
};

struct MediaIdentity {
  std::string media_id;
  std::int64_t size_bytes = 0;
  std::int64_t duration_us = 0;
  std::string container_format;
  std::vector<StreamMetadata> streams;
  std::optional<ChunkProof> chunk_proof;
  Blake3State blake3_state = Blake3State::pending;
  std::string blake3_hash;
  std::string blake3_state_reason;
  std::string original_filename_hint;
};

struct MediaBinding {
  std::string binding_id;
  std::string binding_contract{std::string{kSvpiBindingContract}};
  std::string verification_state{"pending"};
  MediaIdentity identity;
};

struct MediaBindingDocument {
  std::string schema{"svpi.media_binding.v0.1"};
  MediaBinding primary_source;
};

[[nodiscard]] nlohmann::json to_json(const StreamMetadata& metadata);
[[nodiscard]] nlohmann::json to_json(const ChunkProof& proof);
[[nodiscard]] nlohmann::json to_json(const MediaIdentity& identity);
[[nodiscard]] nlohmann::json to_json(const MediaBinding& binding);
[[nodiscard]] nlohmann::json to_json(const MediaBindingDocument& doc);

[[nodiscard]] bool write_media_binding(
    const std::filesystem::path& output_path,
    const MediaBindingDocument& doc);

}  // namespace svp::package
