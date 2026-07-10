#include "svp/package/embedded_svpi.hpp"

#include "embedded_file_io.hpp"

#include <fstream>

namespace svp::package {

EmbeddedSvpiOperationResult extract_embedded_svpi(
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

  detail::TemporaryOutput temporary;
  if (!detail::make_temporary_output(output_path, temporary, error_message)) {
    return detail::failure_result(output_path, std::move(inspection), error_message);
  }
  std::ifstream input(container_path, std::ios::binary);
  std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
  const auto& embedding = inspection.embeddings.front();
  if (!input || !output || !detail::copy_bytes(
          input, output, embedding.payload_offset, embedding.payload_size)) {
    return detail::failure_result(
        output_path, std::move(inspection),
        "Failed while extracting embedded SVPI bytes.");
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
