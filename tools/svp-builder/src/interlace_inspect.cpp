#include "svp/builder/interlace.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/package/package_summary.hpp"

#include <nlohmann/json.hpp>

namespace svp::builder {

InterlaceInspectResult interlace_inspect(const InterlaceInspectOptions& options) {
  InterlaceInspectResult result;

  const std::filesystem::path svpi_path(options.svpi_path);
  if (!std::filesystem::exists(svpi_path)) {
    result.error_message = "SVPI file does not exist: " + options.svpi_path;
    return result;
  }

  auto summary = svp::package::read_package_summary(svpi_path);
  if (!summary.layout_readable) {
    result.error_message = "could not read SVPI layout: " + summary.layout_error_message;
    return result;
  }

  result.success = true;
  result.artifact_type = summary.svpi.is_svpi ? "SVPI" : "SVP";
  result.entry_count = summary.entry_count;
  result.root_entries = summary.root_entries;

  if (summary.svpi.is_svpi) {
    result.svpi_version = summary.svpi.svpi_version;
    result.manifest_format = "svpi";
    result.has_media_original = summary.svpi.has_media_original;
    result.binding_id = summary.svpi.media_binding.binding_id;
    result.binding_contract = summary.svpi.media_binding.binding_contract;
    result.binding_verification_state = summary.svpi.media_binding.verification_state;
    result.blake3_state = summary.svpi.media_binding.blake3_state;
    result.blake3_hash = summary.svpi.media_binding.blake3_hash;
    result.media_id = summary.svpi.media_binding.media_id;
    result.size_bytes = summary.svpi.media_binding.size_bytes;
    result.duration_us = summary.svpi.media_binding.duration_us;
    result.container_format = summary.svpi.media_binding.container_format;
    result.original_filename_hint = summary.svpi.media_binding.original_filename_hint;
  }

  result.has_index_sqlite = summary.index.sqlite_present;
  result.has_index_manifest = summary.index.file.present;

  auto layout_result = svp::package::read_package_layout(svpi_path);
  if (layout_result.has_value()) {
    const auto& layout = layout_result.value();
    result.has_provenance = layout.has_entry("provenance/processors.jsonl") &&
                           layout.has_entry("provenance/interlace_events.jsonl");
  }

  result.recombination_ready =
      result.has_index_sqlite &&
      result.has_index_manifest &&
      result.has_provenance &&
      !result.has_media_original &&
      summary.svpi.is_svpi;

  return result;
}

}  // namespace svp::builder
