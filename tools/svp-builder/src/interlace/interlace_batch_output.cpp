#include "svp/builder/interlace_batch.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace svp::builder {

std::string_view batch_output_format_name(BatchOutputFormat format) noexcept {
  switch (format) {
    case BatchOutputFormat::svpi:
      return "svpi";
    case BatchOutputFormat::embedded_svpi:
      return "embedded-svpi";
  }
  return "svpi";
}

std::optional<BatchOutputFormat> parse_batch_output_format(
    std::string_view format) noexcept {
  if (format == "svpi") {
    return BatchOutputFormat::svpi;
  }
  if (format == "embedded-svpi") {
    return BatchOutputFormat::embedded_svpi;
  }
  return std::nullopt;
}

std::filesystem::path resolve_batch_artifact_path(
    const std::filesystem::path& media_path,
    const std::filesystem::path& source_dir,
    const std::filesystem::path& out_dir,
    BatchOutputFormat format,
    SidecarVisibility visibility) {
  if (format == BatchOutputFormat::svpi) {
    return resolve_sidecar_path(media_path, visibility, out_dir);
  }

  const auto relative_path = std::filesystem::relative(media_path, source_dir);
  return out_dir / relative_path;
}

}  // namespace svp::builder
