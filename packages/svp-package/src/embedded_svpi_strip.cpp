#include "svp/package/embedded_svpi.hpp"

#include "embedded_file_io.hpp"
#include "embedded_isobmff_copy.hpp"
#include "isobmff_top_level.hpp"

#include <array>
#include <fstream>

namespace svp::package {

EmbeddedSvpiOperationResult strip_embedded_svpi(
    const std::filesystem::path& container_path,
    const std::filesystem::path& output_path,
    bool overwrite_output) {
  auto inspection = inspect_embedded_svpi(container_path, true);
  if (!inspection.has_single_valid_embedding() ||
      !inspection.embeddings.front().hash_verified ||
      !inspection.embeddings.front().hash_matches) {
    return detail::failure_result(
        output_path, std::move(inspection),
        "Embedded SVPI envelope or payload hash is invalid.");
  }
  std::string error_message;
  if (!detail::output_is_allowed(output_path, overwrite_output, error_message)) {
    return detail::failure_result(output_path, std::move(inspection), error_message);
  }

  const auto scan = detail::scan_top_level_boxes(container_path);
  detail::TemporaryOutput temporary;
  if (!detail::make_temporary_output(output_path, temporary, error_message)) {
    return detail::failure_result(output_path, std::move(inspection), error_message);
  }
  std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
  const std::array<std::uint8_t, 32> unused_hash{};
  if (!output || !detail::copy_iso_bmff_with_embedding_change(
          container_path, scan, inspection.embeddings, output, scan.file_size,
          nullptr, 0, unused_hash)) {
    return detail::failure_result(
        output_path, std::move(inspection),
        "Failed while stripping embedded SVPI box.");
  }
  output.close();
  if (!output || !detail::finalize_temporary_output(
                     temporary, output_path, error_message)) {
    return detail::failure_result(output_path, std::move(inspection), error_message);
  }
  return {.success = true, .output_path = output_path,
          .inspection = std::move(inspection)};
}

}  // namespace svp::package
