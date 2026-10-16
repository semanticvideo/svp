// svp-builder --max-resolution in pieces (resize/split_resize.hpp): the packet
// report, where a video is cut (clean keyframes on the kPieceSeconds grid,
// whatever the codec's frame order), the cut, encode, and join commands, the
// resize.piece task's parameters and record, and the piece queue. With a
// real ffmpeg and ffprobe ($SVP_FFMPEG / $SVP_FFPROBE or PATH) it also
// resizes long synthetic clips with B-frames, a rotation, video that starts
// after the audio, off-grid frames, and a 1/90000 time base, and proves every
// frame keeps its source time and the resized media is the same bytes
// whichever Macs encoded which pieces; without them those cases print
// "Skipping:".

#include "resize/cancellable_command.hpp"
#include "resize/piece_commands.hpp"
#include "resize/piece_plan.hpp"
#include "resize/piece_task.hpp"
#include "resize/resize_check.hpp"
#include "resize/resize_encoder.hpp"
#include "resize/resized_source.hpp"
#include "resize/source_packets.hpp"
#include "resize/split_resize.hpp"

#include "svp/builder/build_progress.hpp"

#include "svp/exec/blake3_digest.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"
#include "svp/vision/canonical_frame_input.hpp"

#include <algorithm>
#include <atomic>
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

bool contains(const std::vector<std::string>& words, const std::vector<std::string>& run) {
  return std::search(words.begin(), words.end(), run.begin(), run.end()) != words.end();
}

std::string read_file(const fs::path& file) {
  std::ifstream input(file, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

// --- packet report -----------------------------------------------------------

void test_packet_report() {
  const std::optional<rs::PacketTable> table = rs::parse_packet_report(
      "packet,0,25156,K__\npacket,7200,16101,___\npacket,N/A,11456,__D\n"
      "packet,3600,99,_C_\nstream,1/90000,\n");
  require(table && table->packets.size() == 4, "four packets are read");
  require(table->time_base.numerator == 1 && table->time_base.denominator == 90000,
          "the stream's time base is read, past its side data");
  require(table->packets[0].keyframe && *table->packets[0].pts == 0 &&
              table->packets[0].size == 25156 && !table->packets[0].hidden,
          "a keyframe's time, size, and flags are read");
  require(!table->packets[2].pts && table->packets[2].hidden,
          "a packet without a time, hidden by the edit list, is read as such");
  require(table->packets[3].hidden, "a corrupt packet is hidden");
  require(!rs::parse_packet_report("packet,0,1,K__\n"), "a report without a time base is refused");
  require(!rs::parse_packet_report("packet,0,K__\nstream,1/25\n"),
          "a packet line of the wrong shape is refused");
  require(!rs::parse_packet_report("packet,x,1,K__\nstream,1/25\n"),
          "a packet time that is not a number is refused");
  require(!rs::parse_packet_report("frame,0,1,K__\nstream,1/25\n"),
          "another section is refused");
  require(!rs::parse_packet_report("stream,0/25\n"), "a zero time base is refused");
}

// --- where to cut ------------------------------------------------------------

rs::SourcePacket packet(std::int64_t pts, bool keyframe) {
  return rs::SourcePacket{.pts = pts, .size = 1, .keyframe = keyframe};
}

// `seconds` of frames at `fps` in a 1/fps time base, in file order: every
// frame a keyframe (`gop` 1), or GOPs of `gop` frames whose B-frames
// (`b_frames` between references) are stored after the reference they
// follow; an open GOP's leading B-frames are shown before its keyframe.
rs::PacketTable stream(int seconds, int fps, int gop, int b_frames, bool open_gop) {
  rs::PacketTable table{.time_base = {1, fps}};
  const int frames = seconds * fps;
  for (int start = 0; start < frames; start += gop) {
    const int end = std::min(start + gop, frames);
    const bool leading = open_gop && start > 0;
    // The keyframe first; with an open GOP, b_frames shown before it.
    table.packets.push_back(packet(start + (leading ? b_frames : 0), true));
    if (leading) {
      for (int b = 0; b < b_frames; ++b) {
        table.packets.push_back(packet(start + b, false));
      }
    }
    int shown = start + 1 + (leading ? b_frames : 0);
    while (shown < end) {
      const int reference = std::min(shown + b_frames, end - 1);
      table.packets.push_back(packet(reference, false));
      for (int b = shown; b < reference; ++b) {
        table.packets.push_back(packet(b, false));
      }
      shown = reference + 1;
    }
  }
  return table;
}

std::vector<std::int64_t> cut_times(const rs::PacketTable& table, const rs::PiecePlan& plan) {
  std::vector<std::int64_t> times;
  for (const std::size_t first : plan.first_packets) {
    times.push_back(*table.packets[first].pts);
  }
  return times;
}

void test_piece_plan() {
  static_assert(rs::kPieceSeconds > 0);
  const std::int64_t piece = rs::kPieceSeconds;

  // Every frame a keyframe: a cut every kPieceSeconds exactly.
  const rs::PacketTable intra = stream(static_cast<int>(3 * piece + 5), 10, 1, 0, false);
  const rs::PiecePlanResult intra_plan = rs::plan_pieces(intra);
  require(intra_plan.plan && cut_times(intra, *intra_plan.plan) ==
                                 std::vector<std::int64_t>{0, 10 * piece, 20 * piece,
                                                           30 * piece},
          "an intra-only video is cut every kPieceSeconds");

  // B-frames stored after their reference, in closed GOPs of 25: a cut at the
  // first keyframe at or after each grid time, and each piece's packets are
  // exactly the frames shown between its cut and the next.
  const rs::PacketTable closed = stream(static_cast<int>(2 * piece + 7), 10, 25, 2, false);
  const rs::PiecePlanResult closed_plan = rs::plan_pieces(closed);
  require(closed_plan.plan.has_value(), "a closed-GOP video with B-frames is cut: " + closed_plan.why_not);
  const std::vector<std::int64_t> closed_cuts = cut_times(closed, *closed_plan.plan);
  require(closed_cuts.size() == 3 && closed_cuts[0] == 0,
          "a closed-GOP video of two pieces and a bit is cut in three");
  for (std::size_t i = 1; i < closed_cuts.size(); ++i) {
    const std::int64_t grid = static_cast<std::int64_t>(i) * 10 * piece;
    require(closed_cuts[i] >= grid && closed_cuts[i] < grid + 25 && closed_cuts[i] % 25 == 0,
            "each cut is the first keyframe at or after its grid time");
  }
  for (std::size_t i = 0; i < closed_cuts.size(); ++i) {
    const std::size_t begin = closed_plan.plan->first_packets[i];
    const std::size_t end = i + 1 < closed_cuts.size() ? closed_plan.plan->first_packets[i + 1]
                                                        : closed.packets.size();
    const std::int64_t limit =
        i + 1 < closed_cuts.size() ? closed_cuts[i + 1] : std::numeric_limits<std::int64_t>::max();
    for (std::size_t p = begin; p < end; ++p) {
      require(*closed.packets[p].pts >= closed_cuts[i] && *closed.packets[p].pts < limit,
              "a piece holds only the frames shown between its cut and the next");
    }
  }

  // An open GOP shows B-frames before its keyframe that are stored after it:
  // no such keyframe is a cut, so the video is resized whole.
  const rs::PacketTable open = stream(static_cast<int>(2 * piece + 7), 10, 25, 2, true);
  const rs::PiecePlanResult open_plan = rs::plan_pieces(open);
  require(!open_plan.plan && open_plan.why_not.find("no keyframe to cut") != std::string::npos,
          "an open-GOP video has no clean cut and is resized whole: " + open_plan.why_not);

  // Mixed: one open GOP at the grid time moves that cut to the next clean
  // keyframe.
  rs::PacketTable mixed = stream(static_cast<int>(2 * piece + 7), 10, 25, 2, false);
  const rs::PiecePlanResult before = rs::plan_pieces(mixed);
  const std::size_t first_cut = before.plan->first_packets[1];
  std::swap(mixed.packets[first_cut], mixed.packets[first_cut + 1]);
  std::swap(mixed.packets[first_cut - 1], mixed.packets[first_cut]);
  const rs::PiecePlanResult after = rs::plan_pieces(mixed);
  require(after.plan && after.plan->first_packets[1] > first_cut,
          "a keyframe whose GOP reaches back is not a cut; the next clean one is");

  // Refused.
  require(rs::plan_pieces(stream(static_cast<int>(piece - 1), 10, 1, 0, false))
                  .why_not.find("no keyframe to cut") != std::string::npos,
          "a video shorter than a piece is resized whole");
  rs::PacketTable untimed = intra;
  untimed.packets[3].pts.reset();
  require(rs::plan_pieces(untimed).why_not.find("no presentation times") != std::string::npos,
          "a video with untimed packets is resized whole");
  rs::PacketTable hidden = intra;
  hidden.packets[3].hidden = true;
  require(rs::plan_pieces(hidden).why_not.find("never shown") != std::string::npos,
          "a video with hidden packets is resized whole");
  rs::PacketTable late_key = intra;
  late_key.packets[0].keyframe = false;
  require(rs::plan_pieces(late_key).why_not.find("does not start") != std::string::npos,
          "a video that does not start on a keyframe is resized whole");
  require(!rs::plan_pieces(rs::PacketTable{.time_base = {1, 25}}).plan,
          "a video without packets is resized whole");

  // A fine time base and a start far from zero change nothing but the times.
  rs::PacketTable shifted{.time_base = {1, 90000}};
  for (const rs::SourcePacket& p : intra.packets) {
    shifted.packets.push_back(packet(*p.pts * 9000 + 123456789, p.keyframe));
  }
  const rs::PiecePlanResult shifted_plan = rs::plan_pieces(shifted);
  require(shifted_plan.plan && shifted_plan.plan->first_packets == intra_plan.plan->first_packets,
          "the cuts depend on the frames' times, not the time base or the start");
}

// --- commands ----------------------------------------------------------------

void test_piece_commands() {
  const rs::ResizeGeometry geometry{.stored_width = 1920,
                                    .stored_height = 1080,
                                    .display_width = 1920,
                                    .display_height = 1080};
  const rs::PieceEncoding demux = rs::piece_encoding(geometry, {1, 24000});
  require(demux.time_base == "1/24000" && demux.stored_width == 1920 &&
              demux.stored_height == 1080,
          "a piece is encoded in the source's time base when MPEG-4 Part 2 can store it");
  require(rs::piece_encoding(geometry, {1, 90000}).time_base == rs::kFineTimeBase,
          "and otherwise in the fine time base, as the one-pass resize is");

  const std::vector<std::string> cut = rs::piece_cut_command(
      "ffmpeg", "in.mp4", 3, {0, 720, 1441}, rs::kPieceContainers[0], "dir/piece%05d.nut");
  require(contains(cut, {"-copyts", "-i", "in.mp4", "-map", "0:3", "-c", "copy"}) &&
              contains(cut, {"-segment_format", "nut", "-segment_frames", "720,1441"}) &&
              contains(cut, {"-reset_timestamps", "0"}) &&
              contains(cut, {"-avoid_negative_ts", "disabled"}),
          "the cut copies the one stream at the planned packets, keeping its times");
  bool refused = false;
  try {
    (void)rs::piece_cut_command("ffmpeg", "in.mp4", 0, {0}, rs::kPieceContainers[0], "p");
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "one piece is not a cut");

  const std::vector<std::string> encode =
      rs::piece_encode_command("ffmpeg", "piece.nut", demux, "encoded.mov");
  require(contains(encode, {"-copyts", "-noautorotate", "-i", "piece.nut"}) &&
              contains(encode, {"-vf", "scale=1920:1080", "-c:v", "mpeg4", "-q:v",
                                std::string(rs::kMpeg4Quantizer), "-threads",
                                std::string(rs::kEncoderThreads), "-fps_mode", "passthrough",
                                "-enc_time_base", "1/24000"}),
          "a piece is encoded as the one-pass resize encodes, at its source times");

  const std::string list = rs::piece_concat_list({"a.mov", "b.mov", "c.mov"}, {0, 30030000, 60060001});
  require(list == "ffconcat version 1.0\nfile 'a.mov'\nduration 30.030000\nfile 'b.mov'\n"
                  "duration 30.030001\nfile 'c.mov'\n",
          "each piece lasts until the next one's start: " + list);
  refused = false;
  try {
    (void)rs::piece_concat_list({"../a.mov"}, {0});
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "a piece name is a plain file name");

  const std::vector<std::string> join = rs::piece_join_command(
      "ffmpeg",
      rs::JoinInput{.concat_list = "dir/pieces.ffconcat",
                    .source = "in.mov",
                    .source_video_index = 2,
                    .first_frame_offset_us = 1459792,
                    .rotation_degrees = 90,
                    .container = rs::ResizeContainer::quicktime},
      "out.mov");
  require(contains(join, {"-display_rotation:v:0", "90", "-itsoffset", "1.459792", "-f", "concat",
                          "-i", "dir/pieces.ffconcat", "-i", "in.mov", "-map", "0:v:0", "-map",
                          "1:a?", "-c", "copy"}) &&
              contains(join, {"-map_metadata", "1", "-map_metadata:s:v:0", "1:s:2"}) &&
              contains(join, {"-f", "mov", "-y", "out.mov"}),
          "the join lays the pieces at the source's start, with its rotation, audio, and "
          "metadata");
  const std::vector<std::string> unrotated = rs::piece_join_command(
      "ffmpeg", rs::JoinInput{.concat_list = "l", .source = "in.mp4",
                              .container = rs::ResizeContainer::mp4},
      "out.mp4");
  require(std::find(unrotated.begin(), unrotated.end(), "-display_rotation:v:0") ==
                  unrotated.end() &&
              contains(unrotated, {"-itsoffset", "0.000000"}) &&
              contains(unrotated, {"-f", "mp4"}),
          "an unrotated MP4 source is joined to an MP4 without a rotation");
}

// --- resize.piece ------------------------------------------------------------

void test_piece_task() {
  const rs::ResizePieceParameters parameters{
      .encoding = {.stored_width = 1920, .stored_height = 1080, .time_base = "1/24000"},
      .ffmpeg_build = "ffmpeg-9.0.2-lgpl"};
  const nlohmann::json json = rs::resize_piece_parameters_to_json(parameters);
  const rs::ResizePieceParameters back = rs::resize_piece_parameters_from_json(json);
  require(back.encoding.stored_width == 1920 && back.encoding.stored_height == 1080 &&
              back.encoding.time_base == "1/24000" && back.ffmpeg_build == parameters.ffmpeg_build,
          "parameters round-trip");
  require(!rs::validate_resize_piece_parameters(json), "they are valid");
  nlohmann::json odd = json;
  odd["encoding"]["stored_width"] = 1921;
  require(rs::validate_resize_piece_parameters(odd).has_value(), "an odd side is refused");
  nlohmann::json fine = json;
  fine["encoding"]["time_base"] = "1/90000";
  require(rs::validate_resize_piece_parameters(fine).has_value(),
          "a time base MPEG-4 Part 2 cannot store is refused");
  nlohmann::json extra = json;
  extra["threads"] = 4;
  require(rs::validate_resize_piece_parameters(extra).has_value(), "an unknown member is refused");
  nlohmann::json no_build = json;
  no_build["ffmpeg_build"] = "";
  require(rs::validate_resize_piece_parameters(no_build).has_value(),
          "a piece names the coordinator's ffmpeg build");

  const svp::exec::Blake3Digest digest = svp::exec::blake3_digest(std::string("piece"));
  const svp::exec::TaskSpec spec = rs::make_resize_piece_spec(
      "resize_1_2", "piece_7", svp::exec::ArtifactRef{.blake3 = digest, .bytes = 5}, parameters);
  require(spec.task_type == rs::kResizePieceTaskType &&
              spec.task_type_version == rs::kResizePieceTaskTypeVersion &&
              spec.inputs.at(std::string(rs::kResizePieceInput)).blake3 == digest &&
              spec.parameters == json,
          "the spec names the piece and its parameters");

  const rs::EncodedPieceRecord record{.blake3 = digest, .bytes = 123};
  const rs::EncodedPieceRecord decoded =
      rs::decode_encoded_piece_record(rs::encode_encoded_piece_record(record));
  require(decoded.blake3 == digest && decoded.bytes == 123, "the record round-trips");
  bool refused = false;
  try {
    (void)rs::decode_encoded_piece_record(R"({"blob":{"blake3":"zz","bytes":1}})");
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "a record without a BLAKE3 digest is refused");
}

// --- the queue ---------------------------------------------------------------

void test_piece_queue() {
  const TemporaryDirectory scratch("svp-piece-queue");
  std::vector<rs::PieceJob> jobs;
  for (std::size_t i = 0; i < 3; ++i) {
    const fs::path piece = scratch.path() / ("p" + std::to_string(i));
    std::ofstream(piece) << "x";
    jobs.push_back({.index = i, .piece = piece, .encoded = scratch.path() / ("e" + std::to_string(i))});
  }
  rs::PieceQueue queue(jobs);
  const std::optional<rs::PieceJob> first = queue.take();
  const std::optional<rs::PieceJob> second = queue.take();
  require(first && first->index == 0 && second && second->index == 1, "pieces go out in order");
  queue.give_back(0);
  const std::optional<rs::PieceJob> again = queue.take();
  require(again && again->index == 0, "a piece given back goes out first again");
  queue.encoded(0);
  queue.encoded(0);
  require(!fs::exists(jobs[0].piece) && fs::exists(jobs[1].piece),
          "an encoded piece's cut file is removed");
  queue.give_back(0);
  const std::optional<rs::PieceJob> third = queue.take();
  require(third && third->index == 2, "an encoded piece is never handed out again");
  // Every piece is out: take() waits until one comes back or all are done.
  std::atomic<bool> got_none = false;
  std::thread waiter([&] { got_none = !queue.take().has_value(); });
  queue.encoded(1);
  queue.encoded(2);
  waiter.join();
  require(got_none && queue.all_encoded(), "take() ends once every piece is encoded");

  bool refused = false;
  try {
    rs::PieceQueue misnumbered(std::vector<rs::PieceJob>{jobs[1]});
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  require(refused, "pieces are numbered from 0");
  jobs[1].index = 0;
  rs::PieceQueue closed(std::vector<rs::PieceJob>{jobs[1]});
  closed.close();
  require(!closed.take() && closed.closed() && !closed.all_encoded(),
          "a closed queue hands out nothing");
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

struct LongClip {
  // The encoder's time base (1/N): MPEG-4 Part 2 stores at most 1/65535.
  int timebase = 30000;
  int ticks_per_frame = 3000;
  int jitter = 7;
  // The container's (1/N), when it is finer than the encoder's.
  int track_timescale = 30000;
  int rotation = 90;
  std::string extension = ".mov";
};

// Two pieces and a bit of 10 fps MPEG-4 Part 2 with two B-frames between
// references in closed GOPs of 25 frames, its video starting half a second
// after its audio, every seventh frame shifted off the grid by `jitter`
// ticks of 1/`timebase`.
fs::path write_long_clip(const Tools& tools, const fs::path& dir, const std::string& name,
                         const LongClip& shape) {
  const fs::path clip = dir / (name + shape.extension);
  const std::string seconds = std::to_string(2 * rs::kPieceSeconds + 7);
  const std::string expression = "N*" + std::to_string(shape.ticks_per_frame) +
                                 "+if(eq(mod(N,7),3)," + std::to_string(shape.jitter) + ",0)+" +
                                 std::to_string(shape.timebase / 2);
  std::vector<std::string> command{tools.ffmpeg.string(), "-nostdin", "-loglevel", "error", "-y"};
  if (shape.rotation != 0) {
    command.insert(command.end(), {"-display_rotation", std::to_string(shape.rotation)});
  }
  command.insert(
      command.end(),
      {"-noautorotate", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=10:duration=" + seconds,
       "-f", "lavfi", "-i", "sine=frequency=440:duration=" + seconds, "-map", "0:v", "-map",
       "1:a", "-vf",
       "settb=1/" + std::to_string(shape.timebase) + ",setpts='" + expression + "'",
       "-fps_mode", "passthrough", "-enc_time_base", "1/" + std::to_string(shape.timebase),
       "-video_track_timescale", std::to_string(shape.track_timescale), "-c:v", "mpeg4", "-q:v", "5",
       "-bf", "2", "-g", "25", "-flags", "+cgop", "-sc_threshold", "1000000000", "-c:a",
       shape.extension == ".mp4" ? "aac" : "pcm_s16le", clip.string()});
  run_or_fail(command, dir, "writing " + name);
  return clip;
}

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

bool same_frame_times(const std::vector<double>& source, const std::vector<double>& resized,
                      double tolerance_s) {
  if (source.size() != resized.size() || source.empty()) return false;
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (std::abs(source[i] - resized[i]) > tolerance_s) return false;
  }
  return true;
}

std::string digest_of(const fs::path& file) {
  return svp::exec::blake3_hex(svp::exec::blake3_digest(read_file(file)));
}

// Another Mac: encodes the pieces it takes with the command every Mac runs,
// or gives each back.
class OtherMac final : public rs::RemotePieceEncoders {
 public:
  OtherMac(fs::path ffmpeg, bool gives_back) : ffmpeg_(std::move(ffmpeg)), gives_back_(gives_back) {}

  void run(rs::PieceQueue& queue, const rs::PieceEncoding& encoding,
           const std::function<void(const std::string&)>&,
           const svp::exec::CancellationToken* cancellation) override {
    while (const std::optional<rs::PieceJob> job = queue.take()) {
      if (gives_back_) {
        queue.give_back(job->index);
        return;
      }
      const rs::CommandOutcome encoded = rs::run_cancellable_command(
          rs::piece_encode_command(ffmpeg_, job->piece, encoding, job->encoded),
          job->encoded.string() + ".out", job->encoded.string() + ".log", cancellation);
      if (!encoded.succeeded) {
        queue.give_back(job->index);
        return;
      }
      ++pieces;
      queue.encoded(job->index);
    }
  }
  std::atomic<std::size_t> pieces = 0;

 private:
  fs::path ffmpeg_;
  bool gives_back_;
};

rs::SplitResizeRequest split_request(const Tools& tools, const fs::path& source,
                                     const fs::path& output, const fs::path& scratch,
                                     std::shared_ptr<rs::RemotePieceEncoders> remote) {
  const svp::media::MediaIngestPlan plan = svp::media::build_media_ingest_plan(
      source, svp::media::probe_media_with_ffprobe(source, tools.ffprobe));
  const std::optional<rs::ResizeGeometry> geometry =
      rs::plan_resize_geometry(plan.primary_video_stream, 120);
  require(geometry.has_value(), "the clip is larger than the bound");
  fs::create_directories(output.parent_path());
  fs::create_directories(scratch);
  return rs::SplitResizeRequest{.source_path = source,
                                .video = plan.primary_video_stream,
                                .geometry = *geometry,
                                .container = rs::resize_container_for(source),
                                .output_path = output,
                                .ffmpeg_path = tools.ffmpeg,
                                .ffprobe_path = tools.ffprobe,
                                .scratch_dir = scratch,
                                .remote = std::move(remote)};
}

void test_split_resizing_long_clips(const Tools& tools) {
  const TemporaryDirectory scratch("svp-split-resize");
  const fs::path clip = write_long_clip(tools, scratch.path(), "long", {});
  const std::string source_digest = digest_of(clip);
  const std::vector<double> source_times = frame_times(tools, clip, scratch.path());

  // This Mac alone.
  const fs::path alone = scratch.path() / "alone" / "talk.120p.mov";
  const rs::SplitResizeOutcome made = rs::split_resize(
      split_request(tools, clip, alone, scratch.path() / "s1", nullptr));
  require(made.status == rs::SplitResizeOutcome::Status::written,
          "a closed-GOP clip with B-frames is resized in pieces: " + made.why);
  require(made.pieces == 3 && made.remote_pieces == 0, "in three pieces, all on this Mac");
  require(same_frame_times(source_times, frame_times(tools, alone, scratch.path()), 1e-6),
          "every frame keeps its source time, B-frames and the late start included");
  require(digest_of(clip) == source_digest, "the source is untouched");
  require(std::distance(fs::directory_iterator(alone.parent_path()), fs::directory_iterator()) == 1,
          "nothing but the resized media is left beside it");
  const svp::media::MediaIngestPlan plan = svp::media::build_media_ingest_plan(
      clip, svp::media::probe_media_with_ffprobe(clip, tools.ffprobe));
  const svp::media::MediaProbe resized = svp::media::probe_media_with_ffprobe(alone, tools.ffprobe);
  require(resized.audio_streams.size() == 1 && resized.video_streams.size() == 1 &&
              resized.video_streams[0].rotation_degrees == 90,
          "the joined media has the source's audio and rotation");
  require(!rs::resized_media_mismatch(plan, *rs::plan_resize_geometry(plan.primary_video_stream, 120),
                                      alone, tools.ffprobe, scratch.path(), nullptr),
          "the joined media passes the one-pass resize's check");

  // Another Mac encodes pieces too: the same bytes.
  const auto other = std::make_shared<OtherMac>(tools.ffmpeg, false);
  const fs::path shared = scratch.path() / "shared" / "talk.120p.mov";
  const rs::SplitResizeOutcome together =
      rs::split_resize(split_request(tools, clip, shared, scratch.path() / "s2", other));
  require(together.status == rs::SplitResizeOutcome::Status::written &&
              together.remote_pieces == other->pieces,
          "pieces encoded on two Macs are joined: " + together.why);
  require(digest_of(shared) == digest_of(alone),
          "the resized media is the same bytes whichever Macs encoded the pieces");

  // A Mac that gives its piece back: this Mac encodes it.
  const fs::path back = scratch.path() / "back" / "talk.120p.mov";
  const rs::SplitResizeOutcome given_back = rs::split_resize(split_request(
      tools, clip, back, scratch.path() / "s3", std::make_shared<OtherMac>(tools.ffmpeg, true)));
  require(given_back.status == rs::SplitResizeOutcome::Status::written &&
              given_back.remote_pieces == 0 && digest_of(back) == digest_of(alone),
          "a piece given back is encoded here, to the same bytes: " + given_back.why);

  // Through --max-resolution: in pieces, the same bytes.
  svp::builder::NullBuildProgressSink sink;
  const rs::ResizeOutcome through = rs::resize_source(
      {.source_path = clip, .output_path = scratch.path() / "cli" / "talk.svpi",
       .max_resolution = 120, .ffmpeg_path = tools.ffmpeg, .ffprobe_path = tools.ffprobe},
      sink, true);
  require(through.error.empty() && through.geometry && digest_of(through.media_path) == digest_of(alone),
          "--max-resolution resizes a clip that can be cut in pieces: " + through.error);

  // Cancelled: nothing left behind.
  svp::exec::CancellationToken cancelled;
  cancelled.request();
  rs::SplitResizeRequest stop =
      split_request(tools, clip, scratch.path() / "stop" / "talk.120p.mov", scratch.path() / "s4", nullptr);
  stop.cancellation = &cancelled;
  require(rs::split_resize(stop).status == rs::SplitResizeOutcome::Status::cancelled &&
              fs::directory_iterator(scratch.path() / "stop") == fs::directory_iterator(),
          "a cancelled split resize leaves nothing");

  // A 1/90000 MP4 (many Android phones), unrotated: every frame within the
  // fine time base's rounding.
  const fs::path fine = write_long_clip(
      tools, scratch.path(), "fine",
      {.track_timescale = 90000, .rotation = 0, .extension = ".mp4"});
  const fs::path fine_out = scratch.path() / "fine" / "fine.120p.mp4";
  const rs::SplitResizeOutcome fine_made =
      rs::split_resize(split_request(tools, fine, fine_out, scratch.path() / "s5", nullptr));
  require(fine_made.status == rs::SplitResizeOutcome::Status::written,
          "a 1/90000 MP4 is resized in pieces: " + fine_made.why);
  require(same_frame_times(frame_times(tools, fine, scratch.path()),
                           frame_times(tools, fine_out, scratch.path()), 9e-6),
          "every 1/90000 frame keeps its time to within 9 microseconds");
  require(svp::media::probe_media_with_ffprobe(fine_out, tools.ffprobe).format_name.find("mp4") !=
              std::string::npos,
          "an MP4 source is joined to an MP4");
}

}  // namespace

int main() {
  test_packet_report();
  test_piece_plan();
  test_piece_commands();
  test_piece_task();
  test_piece_queue();
  if (const std::optional<Tools> tools = real_tools()) {
    test_split_resizing_long_clips(*tools);
  } else {
    std::cout << "Skipping: split resizing needs ffmpeg and ffprobe "
                 "($SVP_FFMPEG/$SVP_FFPROBE or PATH)\n";
  }
  std::cout << "split resize tests passed\n";
  return 0;
}
