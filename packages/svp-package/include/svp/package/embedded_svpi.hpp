#pragma once

#include "svp/package/iso_bmff_container.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svp::package {

enum class EmbeddedSvpiIssueCode {
  input_unreadable,
  unsupported_container,
  invalid_box_structure,
  truncated_uuid_box,
  unsupported_profile_version,
  invalid_envelope_size,
  unsupported_flags,
  invalid_envelope_magic,
  payload_outside_file,
  payload_length_mismatch,
  payload_hash_mismatch,
  duplicate_embeddings,
  no_embedding,
  unsafe_tail_layout,
  zero_sized_top_level_box,
  output_exists,
  output_write_failed,
};

struct EmbeddedSvpiIssue {
  EmbeddedSvpiIssueCode code = EmbeddedSvpiIssueCode::invalid_box_structure;
  std::uint64_t offset = 0;
  std::string message;
};

struct EmbeddedSvpiInfo {
  std::uint16_t profile_version = 0;
  std::uint16_t envelope_size = 0;
  std::uint32_t flags = 0;
  std::uint64_t box_offset = 0;
  std::uint64_t box_size = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_size = 0;
  std::array<std::uint8_t, 32> expected_hash{};
  std::array<std::uint8_t, 32> actual_hash{};
  bool envelope_valid = false;
  bool hash_verified = false;
  bool hash_matches = false;
};

struct EmbeddedSvpiInspection {
  std::filesystem::path path;
  std::uint64_t file_size = 0;
  std::uint64_t top_level_box_count = 0;
  std::uint64_t scanner_bytes_read = 0;
  bool input_readable = false;
  bool container_structure_valid = false;
  IsoBmffContainerInfo container;
  std::vector<EmbeddedSvpiInfo> embeddings;
  std::vector<EmbeddedSvpiIssue> issues;

  [[nodiscard]] bool has_single_valid_embedding() const noexcept;
};

struct EmbeddedSvpiWriteOptions {
  bool replace_existing = false;
  bool overwrite_output = false;
};

struct EmbeddedSvpiOperationResult {
  bool success = false;
  std::filesystem::path output_path;
  EmbeddedSvpiInspection inspection;
  std::string error_message;
};

[[nodiscard]] EmbeddedSvpiInspection inspect_embedded_svpi(
    const std::filesystem::path& path,
    bool verify_payload_hash = false);

[[nodiscard]] EmbeddedSvpiOperationResult embed_svpi_in_iso_bmff(
    const std::filesystem::path& container_path,
    const std::filesystem::path& svpi_path,
    const std::filesystem::path& output_path,
    const EmbeddedSvpiWriteOptions& options = {});

[[nodiscard]] EmbeddedSvpiOperationResult extract_embedded_svpi(
    const std::filesystem::path& container_path,
    const std::filesystem::path& output_path,
    bool overwrite_output = false);

[[nodiscard]] EmbeddedSvpiOperationResult strip_embedded_svpi(
    const std::filesystem::path& container_path,
    const std::filesystem::path& output_path,
    bool overwrite_output = false);

[[nodiscard]] bool svpi_uuid_box_requires_extended_size(
    std::uint64_t svpi_payload_size) noexcept;

[[nodiscard]] const char* to_string(EmbeddedSvpiIssueCode code) noexcept;

}  // namespace svp::package
