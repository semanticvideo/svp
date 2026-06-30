#include "svp/package/media_binding_factory.hpp"

#include "svp/media/media_probe.hpp"
#include "svp/media/canonical_timing.hpp"

#include <blake3.h>

#include <algorithm>
#include <array>
#include <cstring>
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

std::string extract_volume_hint(const std::filesystem::path& path) {
  const auto root = path.root_name();
  if (!root.empty()) {
    return root.string();
  }
  return {};
}

std::string container_format_from_probe(const svp::media::MediaProbe& probe) {
  if (!probe.format_name.empty()) {
    return probe.format_name;
  }
  return "unknown";
}

std::int64_t duration_us_from_probe(const svp::media::MediaProbe& probe) {
  if (probe.container_timing.has_value() &&
      probe.container_timing->duration_pts.has_value()) {
    const auto& timing = *probe.container_timing;
    const auto& duration = *timing.duration_pts;
    if (timing.timebase.denominator > 0) {
      return svp::media::pts_to_microseconds(duration, timing.timebase);
    }
  }
  return 0;
}

std::vector<StreamMetadata> streams_from_probe(const svp::media::MediaProbe& probe) {
  std::vector<StreamMetadata> streams;

  for (const auto& vs : probe.video_streams) {
    StreamMetadata sm;
    sm.codec_name = vs.codec_name;
    sm.width = vs.width;
    sm.height = vs.height;
    if (vs.timing.average_frame_rate.has_value()) {
      const auto& fr = *vs.timing.average_frame_rate;
      if (fr.denominator > 0) {
        sm.frame_rate = static_cast<double>(fr.numerator) /
                        static_cast<double>(fr.denominator);
      }
    }
    if (vs.timing.duration_pts.has_value() && vs.timing.timebase.denominator > 0) {
      sm.duration_us = svp::media::pts_to_microseconds(
          *vs.timing.duration_pts, vs.timing.timebase);
    }
    streams.push_back(std::move(sm));
  }

  for (const auto& as : probe.audio_streams) {
    StreamMetadata sm;
    sm.codec_name = as.codec_name;
    sm.sample_rate = as.sample_rate;
    sm.channels = as.channels;
    if (as.timing.duration_pts.has_value() && as.timing.timebase.denominator > 0) {
      sm.duration_us = svp::media::pts_to_microseconds(
          *as.timing.duration_pts, as.timing.timebase);
    }
    streams.push_back(std::move(sm));
  }

  return streams;
}

ChunkProof compute_chunk_proof(
    const std::filesystem::path& path,
    std::int64_t chunk_size,
    std::int64_t total_size) {
  ChunkProof proof;
  proof.chunk_size_bytes = chunk_size;

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return proof;
  }

  std::int64_t offset = 0;
  std::int64_t chunk_index = 0;
  std::array<char, 64 * 1024> buffer{};

  while (offset < total_size) {
    const std::int64_t remaining = total_size - offset;
    const std::int64_t this_chunk_size =
        std::min(chunk_size, remaining);

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    std::int64_t chunk_read = 0;
    while (chunk_read < this_chunk_size) {
      const std::int64_t to_read =
          std::min(static_cast<std::int64_t>(buffer.size()),
                   this_chunk_size - chunk_read);
      file.read(buffer.data(), static_cast<std::streamsize>(to_read));
      const std::streamsize count = file.gcount();
      if (count <= 0) {
        break;
      }
      blake3_hasher_update(&hasher, buffer.data(),
                           static_cast<size_t>(count));
      chunk_read += static_cast<std::int64_t>(count);
    }

    std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const uint8_t byte : digest) {
      stream << std::setw(2) << static_cast<int>(byte);
    }

    proof.last_chunk_hash = "blake3:" + stream.str();
    proof.last_chunk_size = this_chunk_size;
    proof.chunk_count = chunk_index + 1;

    offset += this_chunk_size;
    ++chunk_index;
  }

  return proof;
}

}  // namespace

MediaBindingDocument create_media_binding(
    const std::filesystem::path& source_path,
    const MediaBindingFactoryOptions& options) {
  MediaBinding binding;
  binding.binding_id = options.binding_id;
  binding.media_role = "primary_source";
  binding.media_id = options.media_id;
  binding.binding_contract = std::string{kSvpiBindingContract};
  binding.verification_state = "pending";
  binding.size_bytes = file_size_bytes(source_path);

  try {
    const auto probe = svp::media::probe_media_with_ffprobe(
        source_path, options.ffprobe_path);
    binding.duration_us = duration_us_from_probe(probe);
    binding.container_format = container_format_from_probe(probe);
    binding.streams = streams_from_probe(probe);
  } catch (const std::exception&) {
    binding.container_format = "unknown";
  }

  if (options.compute_full_blake3 && binding.size_bytes > 0) {
    binding.identity.blake3_hash = blake3_hex_for_file(source_path);
    if (!binding.identity.blake3_hash.empty()) {
      binding.identity.blake3_state = Blake3State::present;
    } else {
      binding.identity.blake3_state = Blake3State::unavailable;
      binding.identity.blake3_state_reason = "BLAKE3 computation failed";
    }
  } else if (!options.compute_full_blake3) {
    binding.identity.blake3_state = Blake3State::pending;
    binding.identity.blake3_state_reason = "Full-file BLAKE3 not requested";
  } else {
    binding.identity.blake3_state = Blake3State::unavailable;
    binding.identity.blake3_state_reason = "Source file is empty or unreadable";
  }

  if (options.compute_chunk_proof && binding.size_bytes > 0) {
    binding.identity.chunk_proof = compute_chunk_proof(
        source_path, options.chunk_size_bytes, binding.size_bytes);
  }

  binding.location_hints.original_filename = source_path.filename().string();
  binding.location_hints.relative_path = "./" + source_path.filename().string();
  binding.location_hints.original_absolute_path =
      std::filesystem::absolute(source_path).string();
  binding.location_hints.volume_hint =
      extract_volume_hint(source_path);

  MediaBindingDocument doc;
  doc.primary_binding_id = options.binding_id;
  doc.bindings.push_back(std::move(binding));
  return doc;
}

BindingVerificationResult verify_media_binding(
    const std::filesystem::path& candidate_path,
    const MediaBindingDocument& binding_doc) {
  BindingVerificationResult result;
  result.candidate_path = candidate_path.string();

  if (binding_doc.bindings.empty()) {
    result.state = BindingVerificationState::unavailable;
    result.state_label = "unavailable";
    result.failing_checks.push_back("no bindings in media_binding.json");
    return result;
  }

  const auto& binding = binding_doc.bindings[0];

  if (!std::filesystem::exists(candidate_path)) {
    result.state = BindingVerificationState::unavailable;
    result.state_label = "unavailable";
    result.failing_checks.push_back("candidate media file does not exist");
    return result;
  }

  result.candidate_size_bytes = file_size_bytes(candidate_path);

  if (result.candidate_size_bytes != binding.size_bytes) {
    result.state = BindingVerificationState::mismatch;
    result.state_label = "mismatch";
    result.failing_checks.push_back("size_bytes mismatch");
    return result;
  }

  result.passing_checks.push_back("size_bytes");

  if (binding.identity.blake3_state == Blake3State::present &&
      !binding.identity.blake3_hash.empty()) {
    result.candidate_blake3 = blake3_hex_for_file(candidate_path);
    if (result.candidate_blake3.empty()) {
      result.state = BindingVerificationState::unavailable;
      result.state_label = "unavailable";
      result.failing_checks.push_back("could not compute BLAKE3 for candidate");
      return result;
    }
    if (result.candidate_blake3 != binding.identity.blake3_hash) {
      result.state = BindingVerificationState::mismatch;
      result.state_label = "mismatch";
      result.failing_checks.push_back("full_file_blake3 mismatch");
      return result;
    }
    result.passing_checks.push_back("full_file_blake3");
  } else if (binding.identity.blake3_state == Blake3State::pending) {
    result.state = BindingVerificationState::pending;
    result.state_label = "pending";
    result.failing_checks.push_back("full_file_blake3 is pending in binding");
    return result;
  } else {
    result.failing_checks.push_back("full_file_blake3 is unavailable in binding");
  }

  if (binding.duration_us > 0) {
    try {
      const auto probe = svp::media::probe_media_with_ffprobe(candidate_path);
      const auto candidate_duration = duration_us_from_probe(probe);
      if (candidate_duration > 0 && candidate_duration != binding.duration_us) {
        result.state = BindingVerificationState::mismatch;
        result.state_label = "mismatch";
        result.failing_checks.push_back("duration_us mismatch");
        return result;
      }
      if (candidate_duration > 0) {
        result.passing_checks.push_back("duration_us");
      }
    } catch (const std::exception&) {
      result.failing_checks.push_back("could not probe candidate media for duration");
    }
  }

  result.state = BindingVerificationState::verified;
  result.state_label = "verified";
  return result;
}

MediaBindingDocument parse_media_binding_json(const std::string& json_content) {
  MediaBindingDocument doc;
  const auto parsed = nlohmann::json::parse(json_content, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return doc;
  }

  if (parsed.contains("schema") && parsed["schema"].is_string()) {
    doc.schema = parsed["schema"].get<std::string>();
  }
  if (parsed.contains("primary_binding_id") &&
      parsed["primary_binding_id"].is_string()) {
    doc.primary_binding_id = parsed["primary_binding_id"].get<std::string>();
  }

  if (parsed.contains("bindings") && parsed["bindings"].is_array()) {
    for (const auto& j : parsed["bindings"]) {
      MediaBinding binding;
      if (j.contains("binding_id") && j["binding_id"].is_string()) {
        binding.binding_id = j["binding_id"].get<std::string>();
      }
      if (j.contains("media_role") && j["media_role"].is_string()) {
        binding.media_role = j["media_role"].get<std::string>();
      }
      if (j.contains("media_id") && j["media_id"].is_string()) {
        binding.media_id = j["media_id"].get<std::string>();
      }
      if (j.contains("binding_contract") && j["binding_contract"].is_string()) {
        binding.binding_contract = j["binding_contract"].get<std::string>();
      }
      if (j.contains("verification_state") &&
          j["verification_state"].is_string()) {
        binding.verification_state =
            j["verification_state"].get<std::string>();
      }
      if (j.contains("duration_us") && j["duration_us"].is_number_integer()) {
        binding.duration_us = j["duration_us"].get<std::int64_t>();
      }
      if (j.contains("size_bytes") && j["size_bytes"].is_number_integer()) {
        binding.size_bytes = j["size_bytes"].get<std::int64_t>();
      }
      if (j.contains("container_format") &&
          j["container_format"].is_string()) {
        binding.container_format = j["container_format"].get<std::string>();
      }

      if (j.contains("identity") && j["identity"].is_object()) {
        const auto& ident = j["identity"];
        if (ident.contains("full_file_blake3") &&
            ident["full_file_blake3"].is_object()) {
          const auto& b3 = ident["full_file_blake3"];
          if (b3.contains("state") && b3["state"].is_string()) {
            const auto state = b3["state"].get<std::string>();
            if (state == "present") {
              binding.identity.blake3_state = Blake3State::present;
            } else if (state == "pending") {
              binding.identity.blake3_state = Blake3State::pending;
            } else {
              binding.identity.blake3_state = Blake3State::unavailable;
            }
          }
          if (b3.contains("value") && b3["value"].is_string()) {
            binding.identity.blake3_hash = b3["value"].get<std::string>();
          }
          if (b3.contains("reason") && b3["reason"].is_string()) {
            binding.identity.blake3_state_reason =
                b3["reason"].get<std::string>();
          }
        }
      }

      if (j.contains("location_hints") &&
          j["location_hints"].is_object()) {
        const auto& lh = j["location_hints"];
        if (lh.contains("original_filename") &&
            lh["original_filename"].is_string()) {
          binding.location_hints.original_filename =
              lh["original_filename"].get<std::string>();
        }
        if (lh.contains("relative_path") &&
            lh["relative_path"].is_string()) {
          binding.location_hints.relative_path =
              lh["relative_path"].get<std::string>();
        }
        if (lh.contains("original_absolute_path") &&
            lh["original_absolute_path"].is_string()) {
          binding.location_hints.original_absolute_path =
              lh["original_absolute_path"].get<std::string>();
        }
      }

      doc.bindings.push_back(std::move(binding));
    }
  }

  return doc;
}

}  // namespace svp::package
