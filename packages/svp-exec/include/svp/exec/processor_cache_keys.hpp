#pragma once

#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace svp::exec {

// Model identity fields shared by the RC2 §20.3 visual and audio cache keys.
struct CacheKeyModelIdentity {
  std::string model_id;
  std::string model_bundle_id;
  Blake3Digest bundle_blake3{};
  Blake3Digest model_blake3{};
  std::string model_runtime;
  std::string execution_provider;
};

// Media time range in integer microseconds (start_us <= end_us).
struct CacheKeyTimeRangeUs {
  std::int64_t start_us = 0;
  std::int64_t end_us = 0;
};

struct VisualCacheKeyInputs {
  std::string svp_core_schema_version;
  std::string processor_id;
  std::string processor_version;
  CacheKeyModelIdentity model;
  std::string normalized_input_format;
  CacheKeyTimeRangeUs time_range;
  std::vector<std::uint64_t> frame_indices;
  Blake3Digest decoded_pixel_blake3{};
  Blake3Digest processor_parameters_blake3{};
};

struct AudioCacheKeyInputs {
  std::string svp_core_schema_version;
  std::string processor_id;
  std::string processor_version;
  CacheKeyModelIdentity model;
  std::uint64_t sample_rate = 0;
  std::string channel_layout;
  std::string sample_format;
  Blake3Digest audio_chunk_blake3{};
  Blake3Digest processor_parameters_blake3{};
};

// The RC2 §20.3 field lists, in spec order, as the `ordered_fields` array for
// compute_cache_key(). JSON types per field:
//   strings            -> JSON string
//   *_blake3           -> JSON string, 64 lowercase hex (no "b3:" prefix)
//   time_range         -> [start_us, end_us] (integers)
//   frame_indices      -> [i0, i1, ...] (integers, caller's order)
//   sample_rate        -> integer
// Throws ExecError(invalid_value) for an empty string field or a time range
// that is negative or ends before it starts.
[[nodiscard]] nlohmann::json visual_cache_key_fields(
    const VisualCacheKeyInputs& inputs);
[[nodiscard]] nlohmann::json audio_cache_key_fields(
    const AudioCacheKeyInputs& inputs);

[[nodiscard]] Blake3Digest visual_cache_key(const VisualCacheKeyInputs& inputs);
[[nodiscard]] Blake3Digest audio_cache_key(const AudioCacheKeyInputs& inputs);

}  // namespace svp::exec
