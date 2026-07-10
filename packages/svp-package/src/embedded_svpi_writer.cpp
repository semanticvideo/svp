#include "svp/package/embedded_svpi.hpp"

#include "embedded_file_io.hpp"
#include "embedded_mp4_copy.hpp"
#include "mp4_top_level.hpp"
#include "svp/package/svpi_embedding_profile.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace svp::package {
namespace {

bool is_type(const detail::TopLevelBox& box,
             const std::array<char, 4>& type) noexcept {
  return box.type == type;
}

bool validate_embedding_layout(
    const detail::TopLevelScan& scan,
    EmbeddedSvpiInspection& inspection,
    std::string& error_message) {
  for (const auto& box : scan.boxes) {
    if (box.extends_to_eof) {
      inspection.issues.push_back({
          .code = EmbeddedSvpiIssueCode::zero_sized_top_level_box,
          .offset = box.offset,
          .message = "Embedding is rejected because a top-level size-zero box extends to EOF.",
      });
      error_message = "Unsafe MP4 layout: top-level size-zero box extends to EOF.";
      return false;
    }
    if (is_type(box, {'m', 'f', 'r', 'o'})) {
      inspection.issues.push_back({
          .code = EmbeddedSvpiIssueCode::unsafe_tail_layout,
          .offset = box.offset,
          .message = "Top-level mfro is not a supported tail layout.",
      });
      error_message = "Unsafe MP4 tail layout: top-level mfro box.";
      return false;
    }
  }
  return true;
}

bool choose_insertion_offset(const detail::TopLevelScan& scan,
                             EmbeddedSvpiInspection& inspection,
                             std::uint64_t& insertion_offset,
                             std::string& error_message) {
  insertion_offset = scan.file_size;
  const auto mfra = std::find_if(scan.boxes.begin(), scan.boxes.end(),
                                 [](const auto& box) {
                                   return is_type(box, {'m', 'f', 'r', 'a'});
                                 });
  if (mfra == scan.boxes.end()) {
    return true;
  }
  const auto is_embedding_box = [&](const detail::TopLevelBox& box) {
    return std::ranges::any_of(
        inspection.embeddings, [&](const EmbeddedSvpiInfo& embedding) {
          return embedding.box_offset == box.offset &&
                 embedding.box_size == box.size;
        });
  };
  if (!std::ranges::all_of(std::next(mfra), scan.boxes.end(),
                           is_embedding_box)) {
    inspection.issues.push_back({
        .code = EmbeddedSvpiIssueCode::unsafe_tail_layout,
        .offset = mfra->offset,
        .message = "mfra is supported only as the terminal top-level box.",
    });
    error_message = "Unsafe MP4 tail layout: non-terminal mfra box.";
    return false;
  }
  insertion_offset = mfra->offset;
  return true;
}

}  // namespace

bool svpi_uuid_box_requires_extended_size(
    std::uint64_t svpi_payload_size) noexcept {
  constexpr std::uint64_t compact_overhead = 24 + kSvpiMp4EnvelopeSize;
  return svpi_payload_size > kIsoBmffMaxCompactBoxSize - compact_overhead;
}

EmbeddedSvpiOperationResult embed_svpi_in_mp4(
    const std::filesystem::path& mp4_path,
    const std::filesystem::path& svpi_path,
    const std::filesystem::path& output_path,
    const EmbeddedSvpiWriteOptions& options) {
  auto inspection = inspect_embedded_svpi(mp4_path, false);
  if (!inspection.mp4_structure_valid) {
    return detail::failure_result(
        output_path, std::move(inspection),
        "Input MP4 has an invalid top-level box structure.");
  }
  std::string error_message;
  if (!detail::output_is_allowed(
          output_path, options.overwrite_output, error_message)) {
    inspection.issues.push_back({
        .code = EmbeddedSvpiIssueCode::output_exists,
        .offset = 0,
        .message = error_message,
    });
    return detail::failure_result(
        output_path, std::move(inspection), error_message);
  }

  const auto scan = detail::scan_top_level_boxes(mp4_path);
  if (!validate_embedding_layout(scan, inspection, error_message)) {
    return detail::failure_result(
        output_path, std::move(inspection), error_message);
  }
  if (!inspection.embeddings.empty() && !options.replace_existing) {
    return detail::failure_result(
        output_path, std::move(inspection),
        "MP4 already contains embedded SVPI; use explicit replacement.");
  }

  std::uint64_t insertion_offset = 0;
  if (!choose_insertion_offset(
          scan, inspection, insertion_offset, error_message)) {
    return detail::failure_result(
        output_path, std::move(inspection), error_message);
  }
  std::uint64_t payload_size = 0;
  std::array<std::uint8_t, 32> payload_hash{};
  if (!detail::hash_file(svpi_path, payload_size, payload_hash)) {
    return detail::failure_result(
        output_path, std::move(inspection), "Unable to read the SVPI input.");
  }

  detail::TemporaryOutput temporary;
  if (!detail::make_temporary_output(
          output_path, temporary, error_message)) {
    return detail::failure_result(
        output_path, std::move(inspection), error_message);
  }
  std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
  if (!output || !detail::copy_mp4_with_embedding_change(
          mp4_path, scan, inspection.embeddings, output, insertion_offset,
          &svpi_path, payload_size, payload_hash)) {
    return detail::failure_result(
        output_path, std::move(inspection),
        "Failed while writing the embedded MP4.");
  }
  output.close();
  if (!output) {
    return detail::failure_result(
        output_path, std::move(inspection), "Failed to flush temporary output.");
  }

  auto output_inspection = inspect_embedded_svpi(temporary.path, true);
  if (!output_inspection.has_single_valid_embedding() ||
      !output_inspection.embeddings.front().hash_verified ||
      !output_inspection.embeddings.front().hash_matches) {
    return detail::failure_result(
        output_path, std::move(output_inspection),
        "Final embedded MP4 verification failed.");
  }
  if (!detail::finalize_temporary_output(
          temporary, output_path, error_message)) {
    return detail::failure_result(
        output_path, std::move(output_inspection), error_message);
  }
  output_inspection.path = output_path;
  return {.success = true, .output_path = output_path,
          .inspection = std::move(output_inspection)};
}

}  // namespace svp::package
