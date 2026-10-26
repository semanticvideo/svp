#include "capture_provenance.hpp"

#include "build_pipeline_internal.hpp"

#include "svp/media/capture_metadata.hpp"

#include <system_error>

namespace svp::builder {

void write_capture_provenance(const BuildPipelineOptions& options,
                              const std::filesystem::path& staging_dir) {
  // --max-resolution builds from a resized copy, whose container may not keep
  // every camera tag (QuickTime metadata keys among them): the camera's
  // metadata is read from the source as given.
  std::filesystem::path source = options.source_path;
  std::error_code error;
  if (!options.capture_source_path.empty() &&
      std::filesystem::is_regular_file(options.capture_source_path, error)) {
    source = options.capture_source_path;
  }
  const svp::media::CaptureMetadata capture =
      svp::media::read_capture_metadata(source, options.ffprobe_path);
  const std::filesystem::path entry =
      staging_dir / std::filesystem::path(std::string(kCaptureProvenanceEntry));
  if (capture.empty()) {
    // A reused staging directory may hold an earlier build's file.
    std::filesystem::remove(entry, error);
    return;
  }
  write_json_file(entry, svp::media::capture_metadata_to_json(capture));
}

}  // namespace svp::builder
