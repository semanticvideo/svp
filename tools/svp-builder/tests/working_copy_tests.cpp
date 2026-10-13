// svp-builder build --max-resolution: the working copy's size, encoders and
// command, the checks that decide whether it may stand in for the source, the
// command runner, its provenance record, and its place in the build-inputs
// digest. With a real ffmpeg and ffprobe ($SVP_FFMPEG / $SVP_FFPROBE or PATH)
// it also makes copies of synthetic clips with variable frame timing, a
// rotation, and video that starts after the audio, and proves every frame
// keeps its timestamp; without them those cases print "Skipping:".

#include "engine/build_inputs_digest.hpp"
#include "working_copy/cancellable_command.hpp"
#include "working_copy/working_copy.hpp"
#include "working_copy/working_copy_check.hpp"
#include "working_copy/working_copy_encoder.hpp"
#include "working_copy/working_copy_geometry.hpp"
#include "working_copy/working_copy_stage.hpp"

#include "svp/builder/build_progress.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/vision/canonical_frame_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
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
namespace wc = svp::builder::working_copy;

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

// --- size --------------------------------------------------------------------

void test_geometry_bounds_the_displayed_short_side() {
  // 4K landscape to 1080p.
  const auto uhd = wc::plan_working_copy_geometry(video(3840, 2160, 0), 1080);
  require(uhd && uhd->stored_width == 1920 && uhd->stored_height == 1080 &&
              uhd->display_width == 1920 && uhd->display_height == 1080,
          "3840x2160 at 1080 is 1920x1080");

  // A short side at or below the bound: used as is, landscape or portrait.
  require(!wc::plan_working_copy_geometry(video(1920, 1080, 0), 1080),
          "a 1920x1080 video is not copied for 1080");
  require(!wc::plan_working_copy_geometry(video(1080, 1920, 0), 1080),
          "a 1080x1920 video is not copied for 1080");
  require(!wc::plan_working_copy_geometry(video(1920, 1080, 90), 1080),
          "a rotated 1920x1080 video is not copied for 1080");
  require(!wc::plan_working_copy_geometry(video(3840, 2160, 0), 2160),
          "a short side exactly the bound is not copied");

  // Portrait 4K, whether its frames are coded tall or coded wide with a
  // quarter-turn rotation, is analysed 1080 wide and 1920 tall, the frames
  // OCR reads from it today. A rotated copy keeps the stored orientation.
  const auto tall = wc::plan_working_copy_geometry(video(2160, 3840, 0), 1080);
  require(tall && tall->display_width == 1080 && tall->display_height == 1920 &&
              tall->stored_width == 1080 && tall->stored_height == 1920,
          "a 2160x3840 frame at 1080 is 1080x1920");
  for (const int rotation : {90, -90, 270, -270}) {
    const auto rotated = wc::plan_working_copy_geometry(video(3840, 2160, rotation), 1080);
    require(rotated && rotated->display_width == 1080 && rotated->display_height == 1920 &&
                rotated->stored_width == 1920 && rotated->stored_height == 1080,
            "3840x2160 rotated " + std::to_string(rotation) +
                " degrees at 1080 is 1080x1920 displayed, 1920x1080 stored");
  }
  const auto half_turn = wc::plan_working_copy_geometry(video(3840, 2160, 180), 1080);
  require(half_turn && half_turn->stored_width == 1920 && half_turn->stored_height == 1080,
          "a half turn does not swap sides");

  // An odd bound rounds down to an even short side; the long side goes to the
  // nearest even number.
  const auto odd = wc::plan_working_copy_geometry(video(3840, 2160, 0), 1081);
  require(odd && odd->display_height == 1080 && odd->display_width == 1920,
          "an odd bound gives an even short side");
  const auto scope = wc::plan_working_copy_geometry(video(4096, 1716, 0), 1080);
  require(scope && scope->display_height == 1080 && scope->display_width == 2578,
          "4096x1716 at 1080 is 2578x1080 (2577.9 to even)");
  const auto scope_tall = wc::plan_working_copy_geometry(video(1716, 4096, 0), 1080);
  require(scope_tall && scope_tall->display_width == 1080 &&
              scope_tall->display_height == 2578,
          "1716x4096 at 1080 is 1080x2578");

  // Non-square pixels: both stored sides scale by one factor.
  const auto anamorphic = wc::plan_working_copy_geometry(video(1440, 1080, 0), 720);
  require(anamorphic && anamorphic->stored_width == 960 && anamorphic->stored_height == 720,
          "1440x1080 at 720 is 960x720 stored");

  require(!wc::plan_working_copy_geometry(video(0, 0, 0), 1080), "no size, no copy");
  require(wc::kMinimumMaxResolution == svp::media::kCanonicalLongestDisplayDimension,
          "the smallest bound is the canonical raster's longest side");
}

// --- encoders and command ----------------------------------------------------

void test_encoders_and_command() {
  const std::vector<wc::WorkingCopyEncoder> encoders = wc::working_copy_encoders();
  require(!encoders.empty() && encoders.back().name == "mpeg4" &&
              encoders.back().execution_provider == "cpu",
          "the software fallback is tried last");
#if defined(__APPLE__)
  require(encoders.size() == 2 && encoders.front().name == "hevc_videotoolbox" &&
              encoders.front().execution_provider == "videotoolbox",
          "VideoToolbox is preferred on Apple platforms");
  require(contains(encoders.front().options, {"-bsf:v", "filter_units=remove_types=39"}),
          "VideoToolbox's per-run SEI is removed");
#else
  require(encoders.size() == 1, "only the software encoder off Apple platforms");
#endif

  const wc::WorkingCopyGeometry geometry{.stored_width = 1080, .stored_height = 608,
                                         .display_width = 608, .display_height = 1080};
  const std::vector<std::string> command = wc::working_copy_ffmpeg_command(
      "/opt/ffmpeg", "/media/in.mp4", 2, geometry, encoders.back(), "/tmp/out.mov");
  require(command.front() == "/opt/ffmpeg", "the command runs the given ffmpeg");
  require(contains(command, {"-noautorotate", "-i", "/media/in.mp4"}),
          "rotation metadata is kept, not applied");
  require(contains(command, {"-map", "0:2", "-map", "0:a?", "-c:a", "copy"}),
          "the primary video by index, and every audio stream copied");
  require(contains(command, {"-vf", "scale=1080:608"}), "frames are scaled to the stored size");
  require(contains(command, {"-c:v", "mpeg4", "-q:v", std::string(wc::kMpeg4Quantizer)}),
          "the encoder and its quality");
  require(contains(command, {"-fps_mode", "passthrough", "-enc_time_base", "demux"}),
          "every frame keeps its source timestamp");
  require(contains(command, {"-f", "mov", "-y", "/tmp/out.mov"}), "a QuickTime file");
}

// --- start offsets -----------------------------------------------------------

void test_stream_start_offset() {
  require(wc::stream_start_offset_us(
              R"({"programs":[],"streams":[{"start_time":"0.500000"}],)"
              R"("format":{"start_time":"0.000000"}})") == 500000,
          "video half a second after the file start");
  require(wc::stream_start_offset_us(
              R"({"streams":[{"start_time":"0.000000"}],"format":{"start_time":"-0.021333"}})") ==
              21333,
          "a negative file start (audio priming)");
  require(!wc::stream_start_offset_us(R"({"streams":[{}],"format":{"start_time":"0.0"}})"),
          "no stream start time");
  require(!wc::stream_start_offset_us(R"({"streams":[],"format":{"start_time":"0.0"}})"),
          "no stream");
  require(!wc::stream_start_offset_us("not json"), "not JSON");
}

// --- build inputs ------------------------------------------------------------

void test_build_inputs_digest_names_max_resolution_only_when_given() {
  svp::builder::BuildPipelineOptions options;
  options.source_path = "/media/in.mp4";
  options.output_path = "/out/in.svp";
  const svp::exec::SourceFingerprintRecord source{.source_id = "source_000",
                                                  .path = "/media/in.mp4",
                                                  .size_bytes = 3};
  const nlohmann::json plan = {{"schema_version", "test"}};
  const nlohmann::json without =
      svp::builder::engine::describe_build_inputs(options, source, plan, {});
  require(!without["options"].contains("max_resolution"), "no --max-resolution, no field");
  options.max_resolution = 1080;
  const nlohmann::json with =
      svp::builder::engine::describe_build_inputs(options, source, plan, {});
  require(with["options"]["max_resolution"] == 1080,
          "--max-resolution is part of the build inputs");
  require(svp::builder::engine::build_inputs_blake3(with) !=
              svp::builder::engine::build_inputs_blake3(without),
          "a journal of one cannot resume as the other");
}

// --- command runner ----------------------------------------------------------

void test_cancellable_command() {
  const TemporaryDirectory scratch("svp-working-copy-command");
  const fs::path out = scratch.path() / "out";
  const fs::path err = scratch.path() / "err";

  const wc::CommandOutcome ok =
      wc::run_cancellable_command({"/bin/sh", "-c", "echo hello"}, out, err, nullptr);
  std::ifstream stdout_file(out);
  const std::string printed((std::istreambuf_iterator<char>(stdout_file)),
                            std::istreambuf_iterator<char>());
  require(ok.started && ok.succeeded && printed == "hello\n", "stdout goes to its file");

  const wc::CommandOutcome failed = wc::run_cancellable_command(
      {"/bin/sh", "-c", "echo 'no such encoder' >&2; exit 3"}, out, err, nullptr);
  require(failed.started && !failed.succeeded && !failed.cancelled &&
              failed.diagnostics.find("exited with status 3") != std::string::npos &&
              failed.diagnostics.find("no such encoder") != std::string::npos,
          "a failure names its status and standard error: " + failed.diagnostics);

  const wc::CommandOutcome missing = wc::run_cancellable_command(
      {(scratch.path() / "no-such-program").string()}, out, err, nullptr);
  require(!missing.succeeded && !missing.diagnostics.empty(), "a missing program fails");

  svp::exec::CancellationToken cancellation;
  std::thread cancel([&cancellation] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancellation.request();
  });
  const auto started = std::chrono::steady_clock::now();
  const wc::CommandOutcome stopped =
      wc::run_cancellable_command({"/bin/sh", "-c", "sleep 30"}, out, err, &cancellation);
  cancel.join();
  require(stopped.started && stopped.cancelled && !stopped.succeeded,
          "a cancelled build stops its command");
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(10),
          "the command stops promptly");
}

// --- a copy that cannot be fingerprinted --------------------------------------

class CapturingSink : public svp::builder::BuildProgressSink {
 public:
  void emit(const svp::builder::ProgressEvent& event) override { events.push_back(event); }
  std::vector<svp::builder::ProgressEvent> events;
};

void write_script(const fs::path& path, const std::string& body) {
  std::ofstream(path) << "#!/bin/sh\n" << body;
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
}

// An ffmpeg that "writes" the copy as a directory (so ffprobe, faked to
// describe a good copy, passes it, and hashing it fails), and records where.
// The copy cannot be fingerprinted, so the build analyses the source: no
// copy, the reason, a warning, and no directory left behind.
void test_unfingerprintable_copy_falls_back_to_the_source() {
  const TemporaryDirectory scratch("svp-working-copy-fingerprint");
  const fs::path ffmpeg = scratch.path() / "ffmpeg";
  const fs::path ffprobe = scratch.path() / "ffprobe";
  const fs::path written = scratch.path() / "written";
  write_script(ffmpeg, "for last; do :; done\nmkdir -p \"$last\"\necho \"$last\" > '" +
                           written.string() + "'\n");
  write_script(ffprobe,
               "case \"$*\" in\n"
               "  *start_time*) echo '{\"streams\":[{\"start_time\":\"0.000000\"}],"
               "\"format\":{\"start_time\":\"0.000000\"}}' ;;\n"
               "  *) echo '{\"format\":{\"format_name\":\"mov\"},\"streams\":[{\"index\":0,"
               "\"codec_type\":\"video\",\"width\":160,\"height\":120}]}' ;;\n"
               "esac\n");
  const fs::path source = scratch.path() / "source.mp4";
  std::ofstream(source) << "source bytes";
  svp::media::MediaProbe probe;
  probe.video_streams.push_back(video(320, 240, 0));
  const svp::media::MediaIngestPlan plan = svp::media::build_media_ingest_plan(source, probe);
  const auto geometry = wc::plan_working_copy_geometry(plan.primary_video_stream, 120);
  require(geometry && geometry->stored_width == 160 && geometry->stored_height == 120,
          "320x240 at 120 is 160x120");

  CapturingSink sink;
  const wc::WorkingCopyResult result = wc::run_working_copy_stage(
      {.plan = plan, .geometry = *geometry, .ffmpeg_path = ffmpeg, .ffprobe_path = ffprobe},
      sink, /*quiet=*/true);
  require(result.copy == nullptr && !result.cancelled,
          "a copy that cannot be fingerprinted is not used");
  require(result.unavailable_reason.find("cannot be fingerprinted") != std::string::npos,
          "the reason says so: " + result.unavailable_reason);
  require(std::any_of(sink.events.begin(), sink.events.end(),
                      [](const svp::builder::ProgressEvent& event) {
                        return event.kind == svp::builder::ProgressEventKind::warning &&
                               event.stage_id == svp::builder::ProgressStageId::working_copy;
                      }),
          "the stage warns that frames come from the source");
  std::ifstream recorded(written);
  std::string copy_path;
  std::getline(recorded, copy_path);
  require(!copy_path.empty() && !fs::exists(fs::path(copy_path).parent_path()),
          "the copy's directory is removed");
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
  const wc::CommandOutcome outcome =
      wc::run_cancellable_command(command, scratch / "run.out", scratch / "run.err", nullptr);
  require(outcome.succeeded, what + ": " + outcome.diagnostics);
}

// How a clip's frames are coded: their stored size, and the rotation its
// metadata asks for.
struct ClipShape {
  std::string size = "320x240";
  int rotation = 90;
};

// A clip of `shape`, its video starting half a second after its audio, with
// every seventh frame shifted off the 30 fps grid by `jitter` ticks of
// `timebase` (1/N).
fs::path write_clip(const Tools& tools, const fs::path& dir, const std::string& name,
                    int timebase, int ticks_per_frame, int jitter,
                    const ClipShape& shape = {}) {
  const fs::path clip = dir / (name + ".mov");
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
       "pcm_s16le", clip.string()});
  run_or_fail(command, dir, "writing " + name);
  return clip;
}

// Every decoded frame's timestamp of `file`'s first video stream.
std::vector<std::string> frame_times(const Tools& tools, const fs::path& file,
                                     const fs::path& scratch) {
  run_or_fail({tools.ffprobe.string(), "-v", "error", "-select_streams", "v:0", "-show_entries",
               "frame=best_effort_timestamp_time", "-of", "csv=p=0", file.string()},
              scratch, "listing frames of " + file.filename().string());
  std::ifstream input(scratch / "run.out");
  std::vector<std::string> times;
  for (std::string line; std::getline(input, line);) {
    line.erase(std::remove(line.begin(), line.end(), ','), line.end());
    if (!line.empty()) times.push_back(line);
  }
  return times;
}

svp::media::MediaIngestPlan plan_of(const Tools& tools, const fs::path& clip) {
  return svp::media::build_media_ingest_plan(
      clip, svp::media::probe_media_with_ffprobe(clip, tools.ffprobe));
}

void test_working_copies_of_synthetic_clips(const Tools& tools) {
  const TemporaryDirectory scratch("svp-working-copy-clips");
  constexpr int kBound = 120;

  // A time base the software fallback can encode in (1/30000), so every
  // ffmpeg build makes this copy.
  const fs::path clip = write_clip(tools, scratch.path(), "ntsc", 30000, 1000, 7);
  const svp::media::MediaIngestPlan plan = plan_of(tools, clip);
  const auto geometry = wc::plan_working_copy_geometry(plan.primary_video_stream, kBound);
  require(geometry && geometry->stored_width == 160 && geometry->stored_height == 120 &&
              geometry->display_width == 120 && geometry->display_height == 160,
          "the rotated clip's 240-pixel short side at 120 is 120x160");

  fs::path copy_directory;
  std::string first_blake3;
  {
    wc::WorkingCopyResult made = wc::make_working_copy(
        {.plan = plan, .geometry = *geometry, .ffmpeg_path = tools.ffmpeg,
         .ffprobe_path = tools.ffprobe});
    require(made.copy != nullptr, "a copy is made: " + made.unavailable_reason);
    const wc::WorkingCopy& copy = *made.copy;
    copy_directory = copy.path().parent_path();
    require(fs::is_regular_file(copy.path()) && copy.fingerprint().size_bytes ==
                                                    fs::file_size(copy.path()),
            "the copy is fingerprinted");
    first_blake3 = svp::exec::blake3_hex(copy.fingerprint().blake3);
    require(fs::equivalent(copy_directory.parent_path(), fs::temp_directory_path()) &&
                copy_directory.filename().string().starts_with("svp-working-copy-"),
            "the copy lives in a private temporary directory");
    require(frame_times(tools, copy.path(), scratch.path()) ==
                frame_times(tools, clip, scratch.path()),
            "every frame keeps its source timestamp (" + copy.encoder().name + ")");
    require(!wc::working_copy_mismatch(plan, *geometry, copy.path(), tools.ffprobe,
                                       scratch.path(), nullptr),
            "the copy passes its check");
    wc::WorkingCopyGeometry other = *geometry;
    other.stored_width += 2;
    require(wc::working_copy_mismatch(plan, other, copy.path(), tools.ffprobe, scratch.path(),
                                      nullptr)
                .has_value(),
            "a copy of another size is rejected");
    svp::media::MediaIngestPlan unrotated = plan;
    unrotated.primary_video_stream.rotation_degrees = 0;
    require(wc::working_copy_mismatch(unrotated, *geometry, copy.path(), tools.ffprobe,
                                      scratch.path(), nullptr)
                .has_value(),
            "a copy with another rotation is rejected");

    const nlohmann::json record =
        wc::working_copy_processor_record(copy, kBound, plan.primary_video_stream);
    const std::string text = record.dump();
    require(record["id"] == "proc_ffmpeg_working_copy_0001" &&
                record["parameters"]["display_width"] == 120 &&
                record["parameters"]["display_height"] == 160 &&
                record["parameters"]["source_display_width"] == 240 &&
                record["parameters"]["source_display_height"] == 320 &&
                record["parameters"]["max_resolution"] == kBound,
            "the record names the copy's and the source's displayed sizes: " + text);
    require(text.find(scratch.path().string()) == std::string::npos &&
                text.find(copy_directory.string()) == std::string::npos &&
                text.find("<source>") != std::string::npos,
            "the record names no host path");
  }
  require(!fs::exists(copy_directory), "the copy's directory goes with it");

  {
    wc::WorkingCopyResult again = wc::make_working_copy(
        {.plan = plan, .geometry = *geometry, .ffmpeg_path = tools.ffmpeg,
         .ffprobe_path = tools.ffprobe});
    require(again.copy != nullptr &&
                svp::exec::blake3_hex(again.copy->fingerprint().blake3) == first_blake3,
            "the same source makes the same copy on the same Mac");
  }

  svp::exec::CancellationToken cancelled;
  cancelled.request();
  const wc::WorkingCopyResult stopped = wc::make_working_copy(
      {.plan = plan, .geometry = *geometry, .ffmpeg_path = tools.ffmpeg,
       .ffprobe_path = tools.ffprobe, .cancellation = &cancelled});
  require(stopped.cancelled && stopped.copy == nullptr, "a cancelled build makes no copy");

  // A portrait clip coded tall (no rotation metadata) is bounded by its short
  // side too: 240x320 at 120 is 120x160, every frame at its timestamp.
  const fs::path tall = write_clip(tools, scratch.path(), "tall", 30000, 1000, 7,
                                   {.size = "240x320", .rotation = 0});
  const svp::media::MediaIngestPlan tall_plan = plan_of(tools, tall);
  const auto tall_geometry =
      wc::plan_working_copy_geometry(tall_plan.primary_video_stream, kBound);
  require(tall_geometry && tall_geometry->stored_width == 120 &&
              tall_geometry->stored_height == 160 && tall_geometry->display_width == 120 &&
              tall_geometry->display_height == 160,
          "the tall clip's 240-pixel short side at 120 is 120x160");
  {
    const wc::WorkingCopyResult tall_copy = wc::make_working_copy(
        {.plan = tall_plan, .geometry = *tall_geometry, .ffmpeg_path = tools.ffmpeg,
         .ffprobe_path = tools.ffprobe});
    require(tall_copy.copy != nullptr, "a tall clip is copied: " + tall_copy.unavailable_reason);
    require(frame_times(tools, tall_copy.copy->path(), scratch.path()) ==
                frame_times(tools, tall, scratch.path()),
            "every frame of the tall clip keeps its timestamp");
  }

  // A 1/90000 time base with off-grid frames: MPEG-4 Part 2 cannot encode in
  // it (its time base denominator is at most 65535), so the copy exists only
  // where VideoToolbox does. Either way no frame may move.
  const fs::path fine = write_clip(tools, scratch.path(), "fine", 90000, 3000, 1234);
  const svp::media::MediaIngestPlan fine_plan = plan_of(tools, fine);
  const wc::WorkingCopyResult fine_copy = wc::make_working_copy(
      {.plan = fine_plan,
       .geometry = *wc::plan_working_copy_geometry(fine_plan.primary_video_stream, kBound),
       .ffmpeg_path = tools.ffmpeg,
       .ffprobe_path = tools.ffprobe});
  if (fine_copy.copy) {
    require(frame_times(tools, fine_copy.copy->path(), scratch.path()) ==
                frame_times(tools, fine, scratch.path()),
            "every frame keeps its 1/90000 timestamp (" + fine_copy.copy->encoder().name + ")");
    std::cout << "1/90000 clip copied with " << fine_copy.copy->encoder().name << "\n";
  } else {
    require(fine_copy.unavailable_reason.find("mpeg4") != std::string::npos,
            "without a copy the reason names the encoder: " + fine_copy.unavailable_reason);
    std::cout << "1/90000 clip not copied: " << fine_copy.unavailable_reason << "\n";
  }
}

}  // namespace

int main() {
  test_geometry_bounds_the_displayed_short_side();
  test_encoders_and_command();
  test_stream_start_offset();
  test_build_inputs_digest_names_max_resolution_only_when_given();
  test_cancellable_command();
  test_unfingerprintable_copy_falls_back_to_the_source();
  if (const std::optional<Tools> tools = real_tools()) {
    test_working_copies_of_synthetic_clips(*tools);
  } else {
    std::cout << "Skipping: working copies of synthetic clips need ffmpeg and ffprobe "
                 "($SVP_FFMPEG/$SVP_FFPROBE or PATH)\n";
  }
  std::cout << "working copy tests passed\n";
  return 0;
}
