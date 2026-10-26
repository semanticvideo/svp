#pragma once

// provenance/capture.json (RC2 §18.3): what the camera recorded about the shot
// in the source file's own metadata, written by the inventory task when the
// source carries any.

#include "svp/builder/build_pipeline.hpp"

#include <filesystem>
#include <string_view>

namespace svp::builder {

// The staging path of the capture file (a staging scope entry).
inline constexpr std::string_view kCaptureProvenanceEntry = "provenance/capture.json";

// Reads capture metadata from the build's original source and writes it to
// staging. Writes nothing when the source carries none. Capture metadata is
// optional: a source it can't be read from is not an error.
void write_capture_provenance(const BuildPipelineOptions& options,
                              const std::filesystem::path& staging_dir);

}  // namespace svp::builder
