#pragma once

// svp-builder build and interlace create --max-resolution N.
//
// A package describes exactly the media it was built from. So when the
// primary video's displayed short side is longer than N, the builder first
// writes the video resized so that side is N (resize_geometry.hpp), as a new
// ISO base media file beside the output (resize_encoder.hpp,
// resized_media_path), checks it (resize_check.hpp), and builds from that file
// as if it had been given as the source: the package's identity, hash, size,
// dimensions, timing, frames, and coordinates are the resized media's own. The
// source is only read; it is never modified or removed. A video at or below N
// is built as is.
//
// The resized media shares the source's timeline: every frame keeps its
// source time (to within the encoder time base) and the audio is copied
// untouched, so a time in the package is the same moment in either file.
//
// Where the resized media ends up depends on the output: an SVPI sidecar
// describes it, so it stays beside the sidecar; an .svp package or an
// embedded SVPI carries it, so the separate file is removed once that output
// is written (finish_resized_media). After a failed build it stays: --resume
// builds from it again and --fresh writes it again.
//
// If no resized media can be written, the build warns and builds from the
// source, so the package describes the source.

#include "resize/resize_geometry.hpp"

#include "svp/builder/build_progress.hpp"
#include "svp/exec/cancellation_token.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace svp::builder::resize {

struct ResizeRequest {
  std::filesystem::path source_path;
  // The source's precomputed probe (--probe-json), if any.
  std::string probe_json_path;
  // The output artifact (--out); the resized media is written beside it.
  std::filesystem::path output_path;
  int max_resolution = 0;
  std::filesystem::path ffmpeg_path;
  std::filesystem::path ffprobe_path;
  // --resume: resized media already beside the output (left by an earlier
  // build) is built from as is, once it passes the check.
  bool resume = false;
  // --fresh (or build's --overwrite): resized media already beside the output
  // is written again.
  bool overwrite = false;
  const svp::exec::CancellationToken* cancellation = nullptr;
};

struct ResizeOutcome {
  // What the build reads and describes: the resized media, or the source.
  std::filesystem::path media_path;
  // Set when media_path is resized media.
  std::optional<ResizeGeometry> geometry;
  // The build was cancelled while the media was being written.
  bool cancelled = false;
  // Set when the build must not start (for example, resized media from an
  // earlier build is in the way without --resume or --overwrite).
  std::string error;
};

// The `resize` progress stage: started and completed events (completed with
// "not resized" and a warning when no resized media could be written, failed
// when cancelled or refused), and one line on standard error unless `quiet`.
[[nodiscard]] ResizeOutcome resize_source(const ResizeRequest& request, BuildProgressSink& sink,
                                          bool quiet);

// After the build: for an .svp or embedded SVPI output that was written
// (`output_written`), removes the separate resized media it carries. An SVPI
// sidecar's media is kept.
void finish_resized_media(const ResizeOutcome& outcome, bool output_carries_media,
                          bool output_written);

// The exit status of a build cancelled while its media was being resized:
// the one BuildPipeline returns for a cancelled build.
[[nodiscard]] int resize_cancelled_exit_code();

}  // namespace svp::builder::resize
