#include "svp/package/embedded_svpi.hpp"

#include "iso_bmff_container_internal.hpp"
#include "isobmff_top_level.hpp"
#include "svp/package/embedded_svpi_transport_profile.hpp"

#include <blake3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace svp::package {
namespace {

EmbeddedSvpiIssue make_issue(EmbeddedSvpiIssueCode code, std::uint64_t offset,
                             std::string message) {
  return {.code = code, .offset = offset, .message = std::move(message)};
}

bool hash_range(const std::filesystem::path& path, std::uint64_t offset,
                std::uint64_t size, std::array<std::uint8_t, 32>& hash) {
  std::ifstream input(path, std::ios::binary);
  if (!input || offset > static_cast<std::uint64_t>(
                             std::numeric_limits<std::streamoff>::max())) {
    return false;
  }
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input) {
    return false;
  }

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  std::array<char, 1024 * 1024> buffer{};
  std::uint64_t remaining = size;
  while (remaining > 0) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    input.read(buffer.data(), chunk);
    if (input.gcount() != chunk) {
      return false;
    }
    blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(chunk));
    remaining -= static_cast<std::uint64_t>(chunk);
  }
  blake3_hasher_finalize(&hasher, hash.data(), hash.size());
  return true;
}

bool parse_envelope(const detail::TopLevelBox& box,
                    std::ifstream& input,
                    EmbeddedSvpiInspection& inspection) {
  std::array<std::uint8_t, 16> uuid{};
  const auto uuid_offset = box.offset + box.header_size - uuid.size();
  if (!detail::read_exact_at(input, uuid_offset, uuid.data(), uuid.size())) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::truncated_uuid_box, box.offset,
        "Unable to read UUID box user type."));
    return false;
  }
  inspection.scanner_bytes_read += uuid.size();
  if (uuid != kEmbeddedSvpiTransportUuid) {
    return false;
  }

  EmbeddedSvpiInfo info;
  info.box_offset = box.offset;
  info.box_size = box.size;

  const auto envelope_offset = box.offset + box.header_size;
  const auto available = box.size - box.header_size;
  if (available < kEmbeddedSvpiEnvelopeSize) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::truncated_uuid_box, box.offset,
        "SVPI UUID box is too small to contain the Version 1 envelope."));
    inspection.embeddings.push_back(info);
    return true;
  }

  std::array<std::uint8_t, kEmbeddedSvpiEnvelopeSize> envelope{};
  if (!detail::read_exact_at(input, envelope_offset, envelope.data(), envelope.size())) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::truncated_uuid_box, box.offset,
        "Unable to read the complete SVPI embedding envelope."));
    inspection.embeddings.push_back(info);
    return true;
  }
  inspection.scanner_bytes_read += envelope.size();

  info.profile_version = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(envelope[8]) << 8U) | envelope[9]);
  info.envelope_size = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(envelope[10]) << 8U) | envelope[11]);
  info.flags = detail::read_be32(envelope.data() + 12);
  info.payload_size = detail::read_be64(envelope.data() + 16);
  std::copy_n(envelope.data() + 24, info.expected_hash.size(),
              info.expected_hash.begin());

  if (!std::equal(kEmbeddedSvpiEnvelopeMagic.begin(), kEmbeddedSvpiEnvelopeMagic.end(),
                  envelope.begin())) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::invalid_envelope_magic, envelope_offset,
        "SVPI embedding envelope magic is invalid."));
  } else if (info.profile_version != kEmbeddedSvpiProfileVersion) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::unsupported_profile_version, envelope_offset + 8,
        "Unsupported Embedded SVPI Transport profile version."));
  } else if (info.envelope_size != kEmbeddedSvpiEnvelopeSize) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::invalid_envelope_size, envelope_offset + 10,
        "Version 1 SVPI embedding envelope size must be 64 bytes."));
  } else if ((info.flags & ~kEmbeddedSvpiSupportedFlags) != 0) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::unsupported_flags, envelope_offset + 12,
        "SVPI embedding envelope contains unsupported flags."));
  } else if (std::ranges::any_of(envelope.begin() + 56, envelope.end(),
                                 [](std::uint8_t byte) { return byte != 0; })) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::unsupported_flags, envelope_offset + 56,
        "Version 1 SVPI embedding reserved bytes must be zero."));
  } else {
    const auto payload_offset = envelope_offset + info.envelope_size;
    const auto box_payload_bytes = available - info.envelope_size;
    info.payload_offset = payload_offset;
    if (info.payload_size != box_payload_bytes) {
      inspection.issues.push_back(make_issue(
          EmbeddedSvpiIssueCode::payload_length_mismatch, envelope_offset + 16,
          "Declared SVPI payload length does not match the UUID box size."));
    } else if (payload_offset > inspection.file_size ||
               info.payload_size > inspection.file_size - payload_offset) {
      inspection.issues.push_back(make_issue(
          EmbeddedSvpiIssueCode::payload_outside_file, payload_offset,
          "Declared SVPI payload extends outside the ISO BMFF container."));
    } else {
      info.envelope_valid = true;
    }
  }

  if (info.envelope_valid && inspection.input_readable) {
    // Hashing is intentionally performed only when requested by the caller.
  }
  inspection.embeddings.push_back(info);
  return true;
}

}  // namespace

bool EmbeddedSvpiInspection::has_single_valid_embedding() const noexcept {
  return container_structure_valid && container.supported &&
         embeddings.size() == 1 &&
         embeddings.front().envelope_valid;
}

EmbeddedSvpiInspection inspect_embedded_svpi(
    const std::filesystem::path& path, bool verify_payload_hash) {
  EmbeddedSvpiInspection inspection;
  inspection.path = path;

  auto scan = detail::scan_top_level_boxes(path);
  inspection.file_size = scan.file_size;
  inspection.top_level_box_count = scan.boxes.size();
  inspection.scanner_bytes_read = scan.bytes_read;
  inspection.input_readable = scan.readable;
  inspection.container = detail::classify_iso_bmff_container(path, scan);
  inspection.scanner_bytes_read += inspection.container.bytes_read;
  inspection.container_structure_valid = inspection.container.structure_valid;
  inspection.issues = std::move(scan.issues);
  if (!inspection.container.signature_present) {
    if (inspection.input_readable) {
      inspection.issues.clear();
    }
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::unsupported_container, 0,
        inspection.container.diagnostic));
    return inspection;
  }
  if (!scan.valid) {
    return inspection;
  }
  if (!inspection.container.supported) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::unsupported_container, 0,
        inspection.container.diagnostic));
    return inspection;
  }

  std::ifstream input(path, std::ios::binary);
  for (const auto& box : scan.boxes) {
    if (box.type == std::array<char, 4>{'u', 'u', 'i', 'd'}) {
      parse_envelope(box, input, inspection);
    }
  }

  if (inspection.embeddings.empty()) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::no_embedding, 0,
        "No top-level SVPI embedding UUID box was found."));
    return inspection;
  }
  if (inspection.embeddings.size() > 1) {
    inspection.issues.push_back(make_issue(
        EmbeddedSvpiIssueCode::duplicate_embeddings,
        inspection.embeddings[1].box_offset,
        "ISO BMFF container contains more than one SVPI embedding UUID box."));
    return inspection;
  }

  auto& info = inspection.embeddings.front();
  if (verify_payload_hash && info.envelope_valid) {
    info.hash_verified = hash_range(path, info.payload_offset, info.payload_size,
                                    info.actual_hash);
    if (!info.hash_verified) {
      inspection.issues.push_back(make_issue(
          EmbeddedSvpiIssueCode::input_unreadable, info.payload_offset,
          "Unable to read the complete embedded SVPI payload for hashing."));
    } else {
      info.hash_matches = info.actual_hash == info.expected_hash;
      if (!info.hash_matches) {
        inspection.issues.push_back(make_issue(
            EmbeddedSvpiIssueCode::payload_hash_mismatch, info.payload_offset,
            "Embedded SVPI payload BLAKE3 does not match the envelope."));
      }
    }
  }
  return inspection;
}

const char* to_string(EmbeddedSvpiIssueCode code) noexcept {
  switch (code) {
    case EmbeddedSvpiIssueCode::input_unreadable: return "input_unreadable";
    case EmbeddedSvpiIssueCode::unsupported_container: return "unsupported_container";
    case EmbeddedSvpiIssueCode::invalid_box_structure: return "invalid_box_structure";
    case EmbeddedSvpiIssueCode::truncated_uuid_box: return "truncated_uuid_box";
    case EmbeddedSvpiIssueCode::unsupported_profile_version: return "unsupported_profile_version";
    case EmbeddedSvpiIssueCode::invalid_envelope_size: return "invalid_envelope_size";
    case EmbeddedSvpiIssueCode::unsupported_flags: return "unsupported_flags";
    case EmbeddedSvpiIssueCode::invalid_envelope_magic: return "invalid_envelope_magic";
    case EmbeddedSvpiIssueCode::payload_outside_file: return "payload_outside_file";
    case EmbeddedSvpiIssueCode::payload_length_mismatch: return "payload_length_mismatch";
    case EmbeddedSvpiIssueCode::payload_hash_mismatch: return "payload_hash_mismatch";
    case EmbeddedSvpiIssueCode::duplicate_embeddings: return "duplicate_embeddings";
    case EmbeddedSvpiIssueCode::no_embedding: return "no_embedding";
    case EmbeddedSvpiIssueCode::unsafe_tail_layout: return "unsafe_tail_layout";
    case EmbeddedSvpiIssueCode::zero_sized_top_level_box: return "zero_sized_top_level_box";
    case EmbeddedSvpiIssueCode::output_exists: return "output_exists";
    case EmbeddedSvpiIssueCode::output_write_failed: return "output_write_failed";
  }
  return "unknown";
}

}  // namespace svp::package
