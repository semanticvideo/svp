// svp-builder build / interlace create --max-resolution: the resized media's
// size, file name, container, and encoder command, the check it must pass,
// and the command runner. With a real ffmpeg and ffprobe ($SVP_FFMPEG /
// $SVP_FFPROBE or PATH) it also resizes synthetic clips with variable frame
// timing, a rotation, video that starts after the audio, and a 1/90000 time
// base, and proves the resized media is written beside the output, keeps
// every frame's time, leaves the source untouched, and is resumed, replaced,
// or refused as asked; without them those cases print "Skipping:".

#include "resize/cancellable_command.hpp"
#include "resize/resize_check.hpp"
#include "resize/resize_encoder.hpp"
#include "resize/resize_geometry.hpp"
#include "resize/resized_source.hpp"

#include "svp/builder/build_progress.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/vision/canonical_frame_input.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace rs = svp::builder::resize;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& name)
      : path_(fs::temp_directory_path() /
              (name + "-" + std::to_string(static_cast<long long>(::getpid())))) {
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

svp::media::VideoStreamProbe video(int width, int height, int rotation) {
  svp::media::VideoStreamProbe stream;
  stream.width = width;
  stream.height = height;
  stream.rotation_degrees = rotation;
  return stream;
}

bool contains(const std::vector<std::string>& words, const std::vector<std::string>& run) {
  return std::search(words.begin(), words.end(), run.begin(), run.end()) != words.end();
}

class CapturingSink : public svp::builder::BuildProgressSink {
 public:
  void emit(const svp::builder::ProgressEvent& event) override { events.push_back(event); }
  std::vector<svp::builder::ProgressEvent> events;
};

// --- size --------------------------------------------------------------------

void test_geometry_bounds_the_displayed_short_side() {
  const auto uhd = rs::plan_resize_geometry(video(3840, 2160, 0), 1080);
  require(uhd && uhd->stored_width == 1920 && uhd->stored_height == 1080 &&
              uhd->display_width == 1920 && uhd->display_height == 1080,
          "3840x2160 at 1080 is 1920x1080");

  require(!rs::plan_resize_geometry(video(1920, 1080, 0), 1080),
          "a 1920x1080 video is not resized for 1080");
  require(!rs::plan_resize_geometry(video(1080, 1920, 0), 1080),
          "a 1080x1920 video is not resized for 1080");
  require(!rs::plan_resize_geometry(video(1920, 1080, 90), 1080),
          "a rotated 1920x1080 video is not resized for 1080");
  require(!rs::plan_resize_geometry(video(3840, 2160, 0), 2160),
          "a short side exactly the bound is not resized");

  const auto tall = rs::plan_resize_geometry(video(2160, 3840, 0), 1080);
  require(tall && tall->display_width == 1080 && tall->display_height == 1920 &&
              tall->stored_width == 1080 && tall->stored_height == 1920,
          "a 2160x3840 frame at 1080 is 1080x1920");
  for (const int rotation : {90, -90, 270, -270}) {
    const auto rotated = rs::plan_resize_geometry(video(3840, 2160, rotation), 1080);
    require(rotated && rotated->display_width == 1080 && rotated->display_height == 1920 &&
                rotated->stored_width == 1920 && rotated->stored_height == 1080,
            "3840x2160 rotated " + std::to_string(rotation) +
                " degrees at 1080 is 1080x1920 displayed, 1920x1080 stored");
  }
  const auto half_turn = rs::plan_resize_geometry(video(3840, 2160, 180), 1080);
  require(half_turn && half_turn->stored_width == 1920 && half_turn->stored_height == 1080,
          "a half turn does not swap sides");

  const auto odd = rs::plan_resize_geometry(video(3840, 2160, 0), 1081);
  require(odd && odd->display_height == 1080 && odd->display_width == 1920,
          "an odd bound rounds down to an even short side");
}

// --- name, container, and command --------------------------------------------

void test_name_container_and_command() {
  require(rs::resize_container_for("a/talk.mp4") == rs::ResizeContainer::mp4 &&
              rs::resize_container_for("a/talk.M4V") == rs::ResizeContainer::mp4,
          "an MP4 source is resized to MP4");
  require(rs::resize_container_for("a/talk.mov") == rs::ResizeContainer::quicktime &&
              rs::resize_container_for("a/talk.mkv") == rs::ResizeContainer::quicktime,
          "every other source is resized to QuickTime");

  const rs::ResizeGeometry landscape{.stored_width = 1920, .stored_height = 1080,
                                     .display_width = 1920, .display_height = 1080};
  const rs::ResizeGeometry portrait{.stored_width = 1920, .stored_height = 1080,
                                    .display_width = 1080, .display_height = 1920};
  require(rs::resized_media_path("out/talk.svpi", landscape, rs::ResizeContainer::mp4) ==
              fs::path("out/talk.1080p.mp4"),
          "the resized media is named after the output, beside it");
  require(rs::resized_media_path("out/talk.svp", portrait, rs::ResizeContainer::quicktime) ==
              fs::path("out/talk.1080p.mov"),
          "the label is the displayed short side");

  require(rs::resize_encoder_time_base({1, 30000}) == "demux" &&
              rs::resize_encoder_time_base({1001, 30000}) == "demux" &&
              rs::resize_encoder_time_base({1, 65535}) == "demux",
          "a time base MPEG-4 Part 2 can store is kept");
  require(rs::resize_encoder_time_base({1, 90000}) == "1/60000" &&
              rs::resize_encoder_time_base({1, 1000000}) == "1/60000" &&
              rs::resize_encoder_time_base({0, 0}) == "1/60000",
          "a finer (or unknown) time base becomes 1/60000");

  svp::media::VideoStreamProbe source = video(3840, 2160, 0);
  source.index = 2;
  source.timing.timebase = {1, 90000};
  const std::vector<std::string> command = rs::resize_ffmpeg_command(
      "ffmpeg", "in.mov", source, landscape, rs::ResizeContainer::quicktime, "out.mov");
  require(contains(command, {"-map", "0:2"}) && contains(command, {"-map", "0:a?"}) &&
              contains(command, {"-c:a", "copy"}) &&
              contains(command, {"-vf", "scale=1920:1080"}) &&
              contains(command, {"-c:v", "mpeg4", "-q:v", "2", "-threads", "8"}) &&
              contains(command, {"-fps_mode", "passthrough"}) &&
              contains(command, {"-enc_time_base", "1/60000"}) &&
              contains(command, {"-f", "mov"}) && contains(command, {"-noautorotate"}),
          "one MPEG-4 Part 2 command on every platform");
  for (const std::string& word : command) {
    require(word.find("videotoolbox") == std::string::npos, "no platform encoder");
  }
}

void test_stream_start_offset() {
  require(rs::stream_start_offset_us(
              R"({"programs":[],"streams":[{"start_time":"0.500000"}],)"
              R"("format":{"start_time":"0.000000"}})") == 500000,
          "video half a second after the file start");
  require(rs::stream_start_offset_us(
              R"({"streams":[{"start_time":"0.000000"}],"format":{"start_time":"-0.021333"}})") ==
              21333,
          "a negative file start (audio priming)");
  require(!rs::stream_start_offset_us(R"({"streams":[{}],"format":{"start_time":"0.0"}})"),
          "no stream start time");
  require(!rs::stream_start_offset_us("not json"), "not JSON");
}

void test_cancellable_command() {
  const TemporaryDirectory scratch("svp-resize-command");
  const fs::path out = scratch.path() / "out";
  const fs::path err = scratch.path() / "err";

  const rs::CommandOutcome ok =
      rs::run_cancellable_command({"/bin/sh", "-c", "echo hello"}, out, err, nullptr);
  std::ifstream stdout_file(out);
  const std::string printed((std::istreambuf_iterator<char>(stdout_file)),
                            std::istreambuf_iterator<char>());
  require(ok.started && ok.succeeded && printed == "hello\n", "stdout goes to its file");

  const rs::CommandOutcome failed = rs::run_cancellable_command(
      {"/bin/sh", "-c", "echo 'no such encoder' >&2; exit 3"}, out, err, nullptr);
  require(failed.started && !failed.succeeded && !failed.cancelled &&
              failed.diagnostics.find("exited with status 3") != std::string::npos &&
              failed.diagnostics.find("no such encoder") != std::string::npos,
          "a failure names its status and standard error: " + failed.diagnostics);

  svp::exec::CancellationToken cancellation;
  std::thread cancel([&cancellation] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancellation.request();
  });
  const auto started = std::chrono::steady_clock::now();
  const rs::CommandOutcome stopped =
      rs::run_cancellable_command({"/bin/sh", "-c", "sleep 30"}, out, err, &cancellation);
  cancel.join();
  require(stopped.started && stopped.cancelled && !stopped.succeeded,
          "a cancelled build stops its command");
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(10),
          "the command stops promptly");
}

void test_finish_resized_media() {
  const TemporaryDirectory scratch("svp-resize-finish");
  const fs::path media = scratch.path() / "talk.1080p.mp4";
  const auto outcome = [&media]() {
    std::ofstream(media) << "resized";
    return rs::ResizeOutcome{.media_path = media,
                             .geometry = rs::ResizeGeometry{.stored_width = 1920,
                                                            .stored_height = 1080,
                                                            .display_width = 1920,
                                                            .display_height = 1080}};
  };
  rs::finish_resized_media(outcome(), /*output_carries_media=*/false, /*output_written=*/true);
  require(fs::exists(media), "an SVPI sidecar's media stays");
  rs::finish_resized_media(outcome(), true, false);
  require(fs::exists(media), "a failed build keeps the media for --resume");
  rs::finish_resized_media(outcome(), true, true);
  require(!fs::exists(media), "an .svp or embedded SVPI that carries the media drops the file");
  std::ofstream(media) << "source";
  rs::finish_resized_media({.media_path = media}, true, true);
  require(fs::exists(media), "a source that was not resized is never removed");
}

// --- with a real ffmpeg ------------------------------------------------------

struct Tools {
  fs::path ffmpeg;
  fs::path ffprobe;
};

std::optional<Tools> real_tools() {
  const char* ffmpeg = std::getenv("SVP_FFMPEG");
  const char* ffprobe = std::getenv("SVP_FFPROBE");
  Tools tools{ffmpeg != nullptr ? fs::path(ffmpeg) : fs::path("ffmpeg"),
              ffprobe != nullptr ? fs::path(ffprobe) : fs::path("ffprobe")};
  if (!svp::vision::ffmpeg_executable_available(tools.ffmpeg) ||
      !svp::vision::ffmpeg_executable_available(tools.ffprobe)) {
    return std::nullopt;
  }
  return tools;
}

void run_or_fail(const std::vector<std::string>& command, const fs::path& scratch,
                 const std::string& what) {
  const rs::CommandOutcome outcome =
      rs::run_cancellable_command(command, scratch / "run.out", scratch / "run.err", nullptr);
  require(outcome.succeeded, what + ": " + outcome.diagnostics);
}

struct ClipShape {
  std::string size = "320x240";
  int rotation = 90;
  std::string extension = ".mov";
};

// A clip of `shape`, its video starting half a second after its audio, with
// every seventh frame shifted off the 30 fps grid by `jitter` ticks of
// `timebase` (1/N).
fs::path write_clip(const Tools& tools, const fs::path& dir, const std::string& name,
                    int timebase, int ticks_per_frame, int jitter,
                    const ClipShape& shape = {}) {
  const fs::path clip = dir / (name + shape.extension);
  const std::string expression = "N*" + std::to_string(ticks_per_frame) +
                                 "+if(eq(mod(N,7),3)," + std::to_string(jitter) + ",0)+" +
                                 std::to_string(timebase / 2);
  std::vector<std::string> command{tools.ffmpeg.string(), "-nostdin", "-loglevel", "error",
                                   "-y"};
  if (shape.rotation != 0) {
    command.insert(command.end(), {"-display_rotation", std::to_string(shape.rotation)});
  }
  command.insert(
      command.end(),
      {"-noautorotate", "-f", "lavfi", "-i", "testsrc2=size=" + shape.size + ":rate=30:duration=2",
       "-f", "lavfi", "-i", "sine=frequency=440:duration=3", "-map", "0:v", "-map", "1:a", "-vf",
       "settb=1/" + std::to_string(timebase) + ",setpts='" + expression + "'", "-fps_mode",
       "passthrough", "-enc_time_base", "1/" + std::to_string(timebase),
       "-video_track_timescale", std::to_string(timebase), "-c:v", "mjpeg", "-q:v", "3", "-c:a",
       shape.extension == ".mp4" ? "aac" : "pcm_s16le", clip.string()});
  run_or_fail(command, dir, "writing " + name);
  return clip;
}

// Every decoded frame's time, in seconds, of `file`'s first video stream.
std::vector<double> frame_times(const Tools& tools, const fs::path& file,
                                const fs::path& scratch) {
  run_or_fail({tools.ffprobe.string(), "-v", "error", "-select_streams", "v:0", "-show_entries",
               "frame=best_effort_timestamp_time", "-of", "csv=p=0", file.string()},
              scratch, "listing frames of " + file.filename().string());
  std::ifstream input(scratch / "run.out");
  std::vector<double> times;
  for (std::string line; std::getline(input, line);) {
    line.erase(std::remove(line.begin(), line.end(), ','), line.end());
    if (!line.empty()) times.push_back(std::stod(line));
  }
  return times;
}

// Every frame of `resized` at the time of the same frame of `source`, to within
// `tolerance_s`.
bool same_frame_times(const std::vector<double>& source, const std::vector<double>& resized,
                      double tolerance_s) {
  if (source.size() != resized.size() || source.empty()) return false;
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (std::abs(source[i] - resized[i]) > tolerance_s) return false;
  }
  return true;
}

std::string digest_of(const fs::path& file) {
  std::ifstream input(file, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  return svp::exec::blake3_hex(svp::exec::blake3_digest(bytes));
}

rs::ResizeRequest request_for(const Tools& tools, const fs::path& source, const fs::path& output,
                              int bound) {
  return {.source_path = source, .output_path = output, .max_resolution = bound,
          .ffmpeg_path = tools.ffmpeg, .ffprobe_path = tools.ffprobe};
}

void test_resizing_synthetic_clips(const Tools& tools) {
  const TemporaryDirectory scratch("svp-resize-clips");
  constexpr int kBound = 120;
  const fs::path out = scratch.path() / "out";

  // A rotated 320x240 clip in a time base MPEG-4 Part 2 can store.
  const fs::path clip = write_clip(tools, scratch.path(), "ntsc", 30000, 1000, 7);
  const std::string source_digest = digest_of(clip);
  CapturingSink sink;
  const rs::ResizeOutcome made =
      rs::resize_source(request_for(tools, clip, out / "talk.svpi", kBound), sink, true);
  require(made.error.empty() && !made.cancelled && made.geometry,
          "the clip is resized: " + made.error);
  require(made.media_path == out / "talk.120p.mov", "beside the output, named for its size: " +
                                                        made.media_path.string());
  require(made.geometry->display_width == 120 && made.geometry->display_height == 160 &&
              made.geometry->stored_width == 160 && made.geometry->stored_height == 120,
          "the rotated clip's 240-pixel short side at 120 is 120x160");
  require(fs::is_regular_file(made.media_path) &&
              !fs::exists(out / "talk.120p.mov.partial"),
          "the resized media is in place and no partial file is left");
  require(fs::exists(clip) && digest_of(clip) == source_digest, "the source is untouched");
  require(same_frame_times(frame_times(tools, clip, scratch.path()),
                           frame_times(tools, made.media_path, scratch.path()), 1e-6),
          "every frame keeps its source time");
  require(!sink.events.empty() &&
              sink.events.front().stage_id == svp::builder::ProgressStageId::resize,
          "the resize progress stage reports it");
  const svp::media::MediaIngestPlan plan = svp::media::build_media_ingest_plan(
      clip, svp::media::probe_media_with_ffprobe(clip, tools.ffprobe));
  require(!rs::resized_media_mismatch(plan, *made.geometry, made.media_path, tools.ffprobe,
                                      scratch.path(), nullptr),
          "the resized media passes its check");
  rs::ResizeGeometry other = *made.geometry;
  other.stored_width += 2;
  require(rs::resized_media_mismatch(plan, other, made.media_path, tools.ffprobe,
                                     scratch.path(), nullptr)
              .has_value(),
          "media of another size is rejected");

  // Already there: refused, resumed, or replaced, as asked.
  const std::string resized_digest = digest_of(made.media_path);
  const rs::ResizeOutcome refused =
      rs::resize_source(request_for(tools, clip, out / "talk.svpi", kBound), sink, true);
  require(!refused.error.empty() && refused.error.find("--overwrite") != std::string::npos,
          "existing resized media is refused without --resume or --overwrite");
  rs::ResizeRequest resume = request_for(tools, clip, out / "talk.svpi", kBound);
  resume.resume = true;
  const rs::ResizeOutcome resumed = rs::resize_source(resume, sink, true);
  require(resumed.error.empty() && resumed.geometry && resumed.media_path == made.media_path &&
              digest_of(resumed.media_path) == resized_digest,
          "--resume builds from the existing resized media as is");
  rs::ResizeRequest overwrite = request_for(tools, clip, out / "talk.svpi", kBound);
  overwrite.overwrite = true;
  const rs::ResizeOutcome replaced = rs::resize_source(overwrite, sink, true);
  require(replaced.error.empty() && replaced.geometry &&
              digest_of(replaced.media_path) == resized_digest,
          "--overwrite writes it again, the same bytes from the same source");

  // Within the bound: built as is, nothing written.
  const rs::ResizeOutcome within =
      rs::resize_source(request_for(tools, clip, out / "small.svpi", 240), sink, true);
  require(within.error.empty() && !within.geometry && within.media_path == clip &&
              !fs::exists(out / "small.240p.mov"),
          "a video within the bound is built as is");

  // Never over the source.
  const fs::path named_like_output = scratch.path() / "talk.120p.mov";
  fs::copy_file(clip, named_like_output);
  const rs::ResizeOutcome over_source = rs::resize_source(
      request_for(tools, named_like_output, scratch.path() / "talk.svpi", kBound), sink, true);
  require(!over_source.error.empty() &&
              over_source.error.find("would replace the source") != std::string::npos &&
              digest_of(named_like_output) == source_digest,
          "resized media never replaces the source");

  // Cancelled: nothing left behind.
  svp::exec::CancellationToken cancelled;
  cancelled.request();
  rs::ResizeRequest stop = request_for(tools, clip, out / "stopped.svpi", kBound);
  stop.cancellation = &cancelled;
  const rs::ResizeOutcome stopped = rs::resize_source(stop, sink, true);
  require(stopped.cancelled && !fs::exists(out / "stopped.120p.mov") &&
              !fs::exists(out / "stopped.120p.mov.partial"),
          "a cancelled build leaves no resized media");

  // A 1/90000 time base (many Android phones) with off-grid frames: written
  // in 1/60000, every frame within 9 microseconds of its source time.
  const fs::path fine = write_clip(tools, scratch.path(), "fine", 90000, 3000, 1234);
  const rs::ResizeOutcome fine_made =
      rs::resize_source(request_for(tools, fine, out / "fine.svpi", kBound), sink, true);
  require(fine_made.error.empty() && fine_made.geometry,
          "a 1/90000 clip is resized: " + fine_made.error);
  require(same_frame_times(frame_times(tools, fine, scratch.path()),
                           frame_times(tools, fine_made.media_path, scratch.path()), 9e-6),
          "every 1/90000 frame keeps its time to within 9 microseconds");

  // An MP4 source, coded tall: resized to MP4.
  const fs::path tall = write_clip(tools, scratch.path(), "tall", 30000, 1000, 7,
                                   {.size = "240x320", .rotation = 0, .extension = ".mp4"});
  const rs::ResizeOutcome tall_made =
      rs::resize_source(request_for(tools, tall, out / "tall.svp", kBound), sink, true);
  require(tall_made.error.empty() && tall_made.media_path == out / "tall.120p.mp4" &&
              tall_made.geometry->display_width == 120 &&
              tall_made.geometry->display_height == 160,
          "an MP4 clip coded tall is resized to a 120x160 MP4: " + tall_made.error);
  require(svp::media::probe_media_with_ffprobe(tall_made.media_path, tools.ffprobe)
                  .format_name.find("mp4") != std::string::npos,
          "the resized MP4 is an MP4");
}

}  // namespace

int main() {
  test_geometry_bounds_the_displayed_short_side();
  test_name_container_and_command();
  test_stream_start_offset();
  test_cancellable_command();
  test_finish_resized_media();
  if (const std::optional<Tools> tools = real_tools()) {
    test_resizing_synthetic_clips(*tools);
  } else {
    std::cout << "Skipping: resizing synthetic clips needs ffmpeg and ffprobe "
                 "($SVP_FFMPEG/$SVP_FFPROBE or PATH)\n";
  }
  std::cout << "resize tests passed\n";
  return 0;
}
