#include "svp/builder/interlace_batch.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder {

namespace {

bool is_safe_relative_path(const std::filesystem::path& path) {
  if (path.empty() || path == "." || path.is_absolute()) {
    return false;
  }
  for (const auto& component : path) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

}  // namespace

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

  const auto relative_path = media_path.lexically_normal().lexically_relative(
      source_dir.lexically_normal());
  if (!is_safe_relative_path(relative_path)) {
    return {};
  }
  return (out_dir / relative_path).lexically_normal();
}

bool batch_artifact_is_contained(
    const std::filesystem::path& artifact_path,
    const std::filesystem::path& output_directory,
    std::string& error_message) {
  if (artifact_path.empty()) {
    error_message =
        "Could not derive a contained source-relative output path.";
    return false;
  }

  std::error_code error;
  const auto canonical_output = std::filesystem::weakly_canonical(
      output_directory, error);
  if (error) {
    error_message = "Could not resolve the batch output directory: " +
                    error.message();
    return false;
  }
  const auto canonical_parent = std::filesystem::weakly_canonical(
      artifact_path.parent_path(), error);
  if (error) {
    error_message = "Could not resolve the batch artifact directory: " +
                    error.message();
    return false;
  }

  const auto relative_parent = canonical_parent.lexically_relative(
      canonical_output);
  if (relative_parent == ".") {
    return true;
  }
  if (!is_safe_relative_path(relative_parent)) {
    error_message =
        "Batch artifact path resolves outside the requested output directory.";
    return false;
  }
  return true;
}

}  // namespace svp::builder
