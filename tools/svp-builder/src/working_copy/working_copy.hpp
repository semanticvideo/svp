#pragma once

// The analysis working copy of svp-builder build --max-resolution.
//
// When the primary video's displayed short side is longer than
// --max-resolution, the build writes one copy of it scaled so that side fits
// (working_copy_geometry.hpp), with every
// frame at its source timestamp and the audio copied untouched
// (working_copy_encoder.hpp), checks it (working_copy_check.hpp), and decodes
// every analysed frame from it: color and shots, canonical frames, depth,
// OCR, evidence crops, keyframe embeddings, and visual tracking, here and on
// worker Macs, which receive the copy instead of the source. Audio stages
// keep reading the source.
//
// The package still describes the source: its identity, name, duration,
// timing, and dimensions come from the source as without the flag, frame and
// crop coordinates are in source pixels, and provenance/processors.jsonl
// gains one record saying that frames were analysed from the copy and at
// what size (working_copy_processor_record). The copy lives in a private
// temporary directory, outside staging, that is removed when the build ends.

#include "working_copy/working_copy_encoder.hpp"
#include "working_copy/working_copy_geometry.hpp"

#include "svp/exec/cancellation_token.hpp"
#include "svp/exec/journal_records.hpp"
#include "svp/media/media_ingest_plan.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace svp::builder::working_copy {

// A written, checked working copy. Destroying it removes its directory.
class WorkingCopy {
 public:
  WorkingCopy(std::filesystem::path directory, WorkingCopyGeometry geometry,
              WorkingCopyEncoder encoder, svp::exec::SourceFingerprintRecord fingerprint,
              double encode_seconds);
  ~WorkingCopy();
  WorkingCopy(const WorkingCopy&) = delete;
  WorkingCopy& operator=(const WorkingCopy&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return fingerprint_path_; }
  [[nodiscard]] const WorkingCopyGeometry& geometry() const noexcept { return geometry_; }
  [[nodiscard]] const WorkingCopyEncoder& encoder() const noexcept { return encoder_; }
  // Its size and BLAKE3: the artifact worker Macs receive and verify.
  [[nodiscard]] const svp::exec::SourceFingerprintRecord& fingerprint() const noexcept {
    return fingerprint_;
  }
  [[nodiscard]] double encode_seconds() const noexcept { return encode_seconds_; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path fingerprint_path_;
  WorkingCopyGeometry geometry_;
  WorkingCopyEncoder encoder_;
  svp::exec::SourceFingerprintRecord fingerprint_;
  double encode_seconds_ = 0.0;
};

struct WorkingCopyRequest {
  // The source's plan: its path, primary video stream, and rotation.
  const svp::media::MediaIngestPlan& plan;
  WorkingCopyGeometry geometry;
  std::filesystem::path ffmpeg_path;
  std::filesystem::path ffprobe_path;
  const svp::exec::CancellationToken* cancellation = nullptr;
};

struct WorkingCopyResult {
  // Set when frames are to be decoded from the copy.
  std::unique_ptr<WorkingCopy> copy;
  // The build was cancelled while the copy was being written.
  bool cancelled = false;
  // Without a copy: why none could be made (each encoder's failure, or the
  // check that rejected the copy). The source is then analysed as is.
  std::string unavailable_reason;
};

// Writes the copy with the first encoder of working_copy_encoders() that
// succeeds, checks it, and fingerprints it.
[[nodiscard]] WorkingCopyResult make_working_copy(const WorkingCopyRequest& request);

// The provenance/processors.jsonl record of a build whose frames were
// analysed from `copy`: what it is (size, encoder, timing and audio rules),
// for which --max-resolution, and what read it. It names no path or time, so it
// is the same for every build of the same source with the same options on
// the same Mac, local or distributed.
[[nodiscard]] nlohmann::json working_copy_processor_record(
    const WorkingCopy& copy, int max_resolution, const svp::media::VideoStreamProbe& source_video);

// The builder foundation JSON's account of --max-resolution (not packaged):
// whether a copy was used, and its size, encoder, bytes, and encode time, or
// why none was made.
[[nodiscard]] nlohmann::json working_copy_summary(const WorkingCopy* copy,
                                                  const std::string& unavailable_reason);

}  // namespace svp::builder::working_copy
