#include "working_copy/working_copy.hpp"

#include "staging_cleanup.hpp"
#include "working_copy/cancellable_command.hpp"
#include "working_copy/working_copy_check.hpp"

#include "svp/exec/source_fingerprint.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace svp::builder::working_copy {
namespace {

// The journal-style source ID of the copy's fingerprint (it is never
// journaled: the journal fingerprints the source).
constexpr std::string_view kWorkingCopySourceId = "working_copy";

// The package's primary media, as other processor records reference it.
constexpr std::string_view kPrimaryMediaRef = "media/media_000001";

// A private directory under the system temporary directory, never inside
// staging (whose contents stage tasks capture and packages ship). It is
// registered with the builder's exit cleanup, so an interrupted process
// removes it like a default staging directory.
std::filesystem::path make_private_directory() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "svp-working-copy-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw std::runtime_error("cannot create a directory for the working copy in " +
                             std::filesystem::temp_directory_path().string());
  }
  std::filesystem::path directory(pattern);
  staging_cleanup_internal::register_auto_staging(directory);
  return directory;
}

void remove_private_directory(const std::filesystem::path& directory) noexcept {
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  staging_cleanup_internal::unregister_auto_staging(directory);
}

// Removes the directory unless a WorkingCopy took it over.
class DirectoryGuard {
 public:
  explicit DirectoryGuard(std::filesystem::path directory) : directory_(std::move(directory)) {}
  ~DirectoryGuard() {
    if (!released_) remove_private_directory(directory_);
  }
  DirectoryGuard(const DirectoryGuard&) = delete;
  DirectoryGuard& operator=(const DirectoryGuard&) = delete;
  void release() noexcept { released_ = true; }

 private:
  std::filesystem::path directory_;
  bool released_ = false;
};

bool rotated_a_quarter_turn(std::int32_t degrees) {
  const std::int32_t normalized = ((degrees % 360) + 360) % 360;
  return normalized == 90 || normalized == 270;
}

std::string joined(const std::vector<std::string>& words) {
  std::string text;
  for (const std::string& word : words) {
    text += (text.empty() ? "" : " ") + word;
  }
  return text;
}

bool cancelled(const svp::exec::CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->requested();
}

}  // namespace

WorkingCopy::WorkingCopy(std::filesystem::path directory, WorkingCopyGeometry geometry,
                         WorkingCopyEncoder encoder,
                         svp::exec::SourceFingerprintRecord fingerprint, double encode_seconds)
    : directory_(std::move(directory)),
      fingerprint_path_(fingerprint.path),
      geometry_(geometry),
      encoder_(std::move(encoder)),
      fingerprint_(std::move(fingerprint)),
      encode_seconds_(encode_seconds) {}

WorkingCopy::~WorkingCopy() { remove_private_directory(directory_); }

WorkingCopyResult make_working_copy(const WorkingCopyRequest& request) {
  WorkingCopyResult result;
  std::filesystem::path directory;
  try {
    directory = make_private_directory();
  } catch (const std::exception& error) {
    result.unavailable_reason = error.what();
    return result;
  }
  DirectoryGuard guard(directory);
  const std::filesystem::path output = directory / kWorkingCopyFileName;

  std::string failures;
  for (const WorkingCopyEncoder& encoder : working_copy_encoders()) {
    const auto started = std::chrono::steady_clock::now();
    const CommandOutcome encoded = run_cancellable_command(
        working_copy_ffmpeg_command(request.ffmpeg_path, request.plan.source_path,
                                    request.plan.primary_video_stream.index, request.geometry,
                                    encoder, output),
        directory / "encode.out", directory / "encode.log", request.cancellation);
    if (encoded.cancelled || cancelled(request.cancellation)) {
      result.cancelled = true;
      return result;
    }
    if (!encoded.succeeded) {
      failures += (failures.empty() ? "" : "; ") + encoder.name + ": " + encoded.diagnostics;
      continue;
    }
    const double encode_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    // A copy that cannot stand in for the source is not the encoder's doing
    // (geometry, rotation, and start time come from the command), so no
    // other encoder is tried.
    if (const std::optional<std::string> mismatch =
            working_copy_mismatch(request.plan, request.geometry, output,
                                  request.ffprobe_path, directory, request.cancellation)) {
      if (cancelled(request.cancellation)) {
        result.cancelled = true;
        return result;
      }
      result.unavailable_reason = encoder.name + " copy rejected: " + *mismatch;
      return result;
    }
    svp::exec::SourceFingerprintRecord fingerprint =
        svp::exec::fingerprint_source(std::string(kWorkingCopySourceId), output);
    guard.release();
    result.copy = std::make_unique<WorkingCopy>(directory, request.geometry, encoder,
                                                std::move(fingerprint), encode_seconds);
    return result;
  }
  result.unavailable_reason = failures.empty() ? "no encoder to try" : failures;
  return result;
}

nlohmann::json working_copy_processor_record(const WorkingCopy& copy, int max_height,
                                             const svp::media::VideoStreamProbe& source_video) {
  const WorkingCopyGeometry& geometry = copy.geometry();
  const bool swaps = rotated_a_quarter_turn(source_video.rotation_degrees);
  // The command with placeholders for the two host paths.
  const std::vector<std::string> command =
      working_copy_ffmpeg_command("ffmpeg", "<source>", source_video.index, geometry,
                                  copy.encoder(), "<working copy>");
  return {
      {"id", "proc_ffmpeg_working_copy_0001"},
      {"name", "FFmpeg analysis working copy"},
      {"version", "svp-working-copy-v1"},
      {"runtime", "ffmpeg_cli"},
      {"execution_provider", copy.encoder().execution_provider},
      {"command", joined(command)},
      {"input_refs", nlohmann::json::array({std::string(kPrimaryMediaRef)})},
      {"output_refs", nlohmann::json::array()},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", nlohmann::json::array()},
      {"parameters",
       {{"max_height", max_height},
        {"source_display_width", swaps ? source_video.height : source_video.width},
        {"source_display_height", swaps ? source_video.width : source_video.height},
        {"rotation_degrees", source_video.rotation_degrees},
        {"display_width", geometry.display_width},
        {"display_height", geometry.display_height},
        {"stored_width", geometry.stored_width},
        {"stored_height", geometry.stored_height},
        {"video_encoder", copy.encoder().name},
        {"frame_timing", "every source frame at its source timestamp"},
        {"audio", "copied without re-encoding; audio analysis reads the source"}}},
      {"note",
       "Every analysed frame (color and shots, canonical frames, depth, OCR, evidence "
       "crops, keyframe embeddings, visual tracking) was decoded from this working copy, "
       "not from the source. Media identity, timing, and dimensions in this package, and "
       "frame and crop coordinates, describe the source."},
  };
}

nlohmann::json working_copy_summary(const WorkingCopy* copy,
                                    const std::string& unavailable_reason) {
  if (copy == nullptr) {
    return {{"used", false}, {"reason", unavailable_reason}};
  }
  return {{"used", true},
          {"display_width", copy->geometry().display_width},
          {"display_height", copy->geometry().display_height},
          {"video_encoder", copy->encoder().name},
          {"bytes", copy->fingerprint().size_bytes},
          {"encode_seconds", copy->encode_seconds()}};
}

}  // namespace svp::builder::working_copy
