#include "resize/resized_source.hpp"

#include "build_interrupt.hpp"
#include "build_pipeline_internal.hpp"
#include "resize/cancellable_command.hpp"
#include "resize/resize_check.hpp"
#include "resize/resize_encoder.hpp"
#include "staging_cleanup.hpp"

#include "svp/builder/build_pipeline.hpp"
#include "svp/media/media_ingest_plan.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace svp::builder::resize {
namespace {

// A private directory for ffmpeg's and ffprobe's logs, registered with the
// builder's exit cleanup so an interrupted process removes it.
class ScratchDirectory {
 public:
  ScratchDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "svp-resize-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error("cannot create a directory in " +
                               std::filesystem::temp_directory_path().string());
    }
    path_ = pattern;
    staging_cleanup_internal::register_auto_staging(path_);
  }
  ~ScratchDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    staging_cleanup_internal::unregister_auto_staging(path_);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

bool cancelled(const svp::exec::CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->requested();
}

std::string size_text(const ResizeGeometry& geometry) {
  return std::to_string(geometry.display_width) + "x" + std::to_string(geometry.display_height);
}

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
  std::error_code ec;
  if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
    return std::filesystem::equivalent(a, b, ec);
  }
  return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
}

ResizeOutcome refused(ResizeOutcome outcome, std::string error, BuildProgressSink& sink) {
  sink.emit(make_stage_failed(ProgressStageId::resize, error));
  outcome.error = std::move(error);
  return outcome;
}

ResizeOutcome interrupted(ResizeOutcome outcome, BuildProgressSink& sink) {
  sink.emit(make_stage_failed(ProgressStageId::resize, "build interrupted"));
  outcome.cancelled = true;
  return outcome;
}

// No resized media: the build reads and describes the source.
ResizeOutcome from_source(ResizeOutcome outcome, const std::string& why, BuildProgressSink& sink,
                          bool quiet) {
  const std::string warning =
      "no resized media could be written, so the package describes the source: " + why;
  sink.emit(make_warning(ProgressStageId::resize, warning));
  sink.emit(make_stage_completed(ProgressStageId::resize, "not resized"));
  if (!quiet) {
    std::cerr << "svp-builder: warning: " << warning << "\n";
  }
  return outcome;
}

ResizeOutcome resized(ResizeOutcome outcome, const std::filesystem::path& media,
                      const ResizeGeometry& geometry, const std::string& how,
                      BuildProgressSink& sink, bool quiet) {
  outcome.media_path = media;
  outcome.geometry = geometry;
  sink.emit(make_stage_completed(ProgressStageId::resize, size_text(geometry) + " mpeg4"));
  if (!quiet) {
    std::error_code ignored;
    std::cerr << "svp-builder: building from " << media.string() << " (" << size_text(geometry)
              << ", " << std::filesystem::file_size(media, ignored) << " bytes, " << how
              << ")\n";
  }
  return outcome;
}

}  // namespace

ResizeOutcome resize_source(const ResizeRequest& request, BuildProgressSink& sink, bool quiet) {
  ResizeOutcome outcome{.media_path = request.source_path};
  sink.emit(make_stage_started(ProgressStageId::resize));

  svp::media::MediaIngestPlan plan;
  try {
    plan = svp::media::build_media_ingest_plan(
        request.source_path,
        load_or_run_probe(request.source_path.string(), request.probe_json_path,
                          request.ffprobe_path.string()));
  } catch (const std::exception&) {
    // The build reports a source it cannot read, as it does without the flag.
    sink.emit(make_stage_completed(ProgressStageId::resize, "not resized"));
    return outcome;
  }
  const std::optional<ResizeGeometry> geometry =
      plan_resize_geometry(plan.primary_video_stream, request.max_resolution);
  if (!geometry) {
    sink.emit(make_stage_completed(ProgressStageId::resize,
                                   "not resized: the video is within --max-resolution"));
    return outcome;
  }

  const std::filesystem::path target = resized_media_path(
      request.output_path, *geometry, resize_container_for(request.source_path));
  if (same_file(target, request.source_path)) {
    return refused(std::move(outcome),
                   "the resized media would replace the source " + target.string() +
                       "; choose another --out",
                   sink);
  }

  std::optional<ScratchDirectory> scratch;
  try {
    scratch.emplace();
  } catch (const std::exception& error) {
    return from_source(std::move(outcome), error.what(), sink, quiet);
  }

  std::error_code ec;
  if (std::filesystem::exists(target, ec)) {
    if (request.resume) {
      const std::optional<std::string> mismatch = resized_media_mismatch(
          plan, *geometry, target, request.ffprobe_path, scratch->path(), request.cancellation);
      if (cancelled(request.cancellation)) {
        return interrupted(std::move(outcome), sink);
      }
      if (mismatch) {
        return refused(std::move(outcome),
                       "the resized media " + target.string() +
                           " cannot be resumed from: " + *mismatch +
                           "; pass --fresh to write it again",
                       sink);
      }
      return resized(std::move(outcome), target, *geometry, "written by the interrupted build",
                     sink, quiet);
    }
    if (!request.overwrite) {
      return refused(std::move(outcome),
                     target.string() +
                         " already exists; pass --fresh to write it again or --resume to "
                         "build from it",
                     sink);
    }
  }

  // Written under another name and moved into place once checked, so the
  // resized media's path never holds a partial file.
  std::filesystem::create_directories(target.parent_path(), ec);
  const std::filesystem::path partial = target.parent_path() / (target.filename().string() + ".partial");
  const auto remove_partial = [&partial]() {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
  };
  const auto started = std::chrono::steady_clock::now();
  const CommandOutcome encoded = run_cancellable_command(
      resize_ffmpeg_command(request.ffmpeg_path, request.source_path, plan.primary_video_stream,
                            *geometry, resize_container_for(request.source_path), partial),
      scratch->path() / "encode.out", scratch->path() / "encode.log", request.cancellation);
  if (encoded.cancelled || cancelled(request.cancellation)) {
    remove_partial();
    return interrupted(std::move(outcome), sink);
  }
  if (!encoded.succeeded) {
    remove_partial();
    return from_source(std::move(outcome), "ffmpeg mpeg4: " + encoded.diagnostics, sink, quiet);
  }
  const double encode_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (const std::optional<std::string> mismatch = resized_media_mismatch(
          plan, *geometry, partial, request.ffprobe_path, scratch->path(), request.cancellation)) {
    remove_partial();
    if (cancelled(request.cancellation)) {
      return interrupted(std::move(outcome), sink);
    }
    return from_source(std::move(outcome), *mismatch, sink, quiet);
  }
  std::filesystem::rename(partial, target, ec);
  if (ec) {
    remove_partial();
    return from_source(std::move(outcome), "cannot move it into place: " + ec.message(), sink,
                       quiet);
  }
  return resized(std::move(outcome), target, *geometry,
                 "written in " + std::to_string(std::lround(encode_seconds)) + " s", sink, quiet);
}

void finish_resized_media(const ResizeOutcome& outcome, bool output_carries_media,
                          bool output_written) {
  if (!outcome.geometry || !output_carries_media || !output_written) {
    return;
  }
  std::error_code ignored;
  std::filesystem::remove(outcome.media_path, ignored);
}

int resize_cancelled_exit_code() {
  constexpr int kSignalExitStatusBase = 128;
  const int signal_number = build_interrupt_signal();
  return signal_number == 0 ? kBuildFailedExitCode : kSignalExitStatusBase + signal_number;
}

}  // namespace svp::builder::resize
