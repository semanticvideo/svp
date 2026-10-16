#include "resize/split_resize.hpp"

#include "resize/cancellable_command.hpp"
#include "resize/piece_plan.hpp"
#include "resize/resize_check.hpp"
#include "resize/source_packets.hpp"
#include "staging_cleanup.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace svp::builder::resize {
namespace {

using Status = SplitResizeOutcome::Status;

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr std::string_view kConcatListName = "pieces.ffconcat";

// The cut pieces and their encodings, beside the resized media (a source
// can be far larger than the temporary directory's volume allows), removed
// when the resize ends or the builder exits.
class PiecesDirectory {
 public:
  explicit PiecesDirectory(const std::filesystem::path& output) {
    std::string pattern =
        (output.parent_path() / ("." + output.filename().string() + ".pieces-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error("cannot create a directory beside " + output.string());
    }
    path_ = pattern;
    staging_cleanup_internal::register_auto_staging(path_);
  }
  ~PiecesDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    staging_cleanup_internal::unregister_auto_staging(path_);
  }
  PiecesDirectory(const PiecesDirectory&) = delete;
  PiecesDirectory& operator=(const PiecesDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

bool cancelled(const svp::exec::CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->requested();
}

SplitResizeOutcome outcome(Status status, std::string why) {
  return SplitResizeOutcome{.status = status, .why = std::move(why)};
}

std::string piece_name(std::string_view stem, std::size_t index, std::string_view extension) {
  char digits[16];
  std::snprintf(digits, sizeof digits, "%05zu", index);
  return std::string(stem) + digits + "." + std::string(extension);
}

// `ticks` of `time_base` in microseconds, rounded to the nearest.
std::int64_t microseconds(std::int64_t ticks, const svp::media::Rational& time_base) {
  const __int128 scaled = static_cast<__int128>(ticks) * time_base.numerator * kMicrosecondsPerSecond;
  const __int128 denominator = time_base.denominator;
  const __int128 rounded = scaled >= 0 ? (2 * scaled + denominator) / (2 * denominator)
                                       : -((-2 * scaled + denominator) / (2 * denominator));
  return static_cast<std::int64_t>(rounded);
}

// Why piece `piece` does not hold exactly packets [begin, end) of `source`:
// the same count, keyframes, and sizes, each shown at its source time plus
// one offset (a container may move the whole piece).
std::optional<std::string> piece_mismatch(const PacketTable& source, std::size_t begin,
                                          std::size_t end, const PacketTable& piece) {
  if (piece.packets.size() != end - begin) {
    return "it holds " + std::to_string(piece.packets.size()) + " packets, not " +
           std::to_string(end - begin);
  }
  const svp::media::Rational& a = piece.time_base;
  const svp::media::Rational& b = source.time_base;
  std::optional<__int128> offset;
  for (std::size_t i = 0; i < piece.packets.size(); ++i) {
    const SourcePacket& got = piece.packets[i];
    const SourcePacket& want = source.packets[begin + i];
    if (!got.pts || got.hidden || got.keyframe != want.keyframe || got.size != want.size) {
      return "its packet " + std::to_string(i) + " is not the source's";
    }
    // Both times over the common denominator a.den * b.den.
    const __int128 difference = static_cast<__int128>(*got.pts) * a.numerator * b.denominator -
                                static_cast<__int128>(*want.pts) * b.numerator * a.denominator;
    if (offset && *offset != difference) {
      return "its packet " + std::to_string(i) + " is not shown at its source time";
    }
    offset = difference;
  }
  return std::nullopt;
}

// Why the joined video's frames are not the source's: one frame per source
// frame, each as long after the first as in the source, within the encoder
// time base's rounding.
std::optional<std::string> joined_mismatch(const PacketTable& source, const PacketTable& joined) {
  if (joined.packets.size() != source.packets.size()) {
    return "the joined video has " + std::to_string(joined.packets.size()) + " frames, the source " +
           std::to_string(source.packets.size());
  }
  std::vector<std::int64_t> want;
  std::vector<std::int64_t> got;
  for (const SourcePacket& packet : source.packets) {
    want.push_back(*packet.pts);
  }
  for (const SourcePacket& packet : joined.packets) {
    if (!packet.pts) {
      return std::string("a joined frame has no presentation time");
    }
    got.push_back(*packet.pts);
  }
  std::sort(want.begin(), want.end());
  std::sort(got.begin(), got.end());
  const svp::media::Rational& s = source.time_base;
  const svp::media::Rational& j = joined.time_base;
  for (std::size_t i = 0; i < want.size(); ++i) {
    // |(got_i - got_0) * j - (want_i - want_0) * s| <= tolerance, all over
    // the common denominator j.den * s.den, in microseconds.
    const __int128 difference =
        (static_cast<__int128>(got[i] - got[0]) * j.numerator * s.denominator -
         static_cast<__int128>(want[i] - want[0]) * s.numerator * j.denominator) *
        kMicrosecondsPerSecond;
    const __int128 limit = static_cast<__int128>(kStartOffsetToleranceUs) * j.denominator *
                           s.denominator;
    if (difference > limit || difference < -limit) {
      return "joined frame " + std::to_string(i) + " is not at its source time";
    }
  }
  return std::nullopt;
}

// The display matrix of stream `stream` of `file` as ffprobe prints it,
// empty when it has none; nullopt when ffprobe cannot tell.
std::optional<std::string> display_matrix(const SplitResizeRequest& request,
                                          const std::filesystem::path& file,
                                          const std::string& stream) {
  const std::filesystem::path report = request.scratch_dir / "display-matrix.json";
  const CommandOutcome outcome = run_cancellable_command(
      {request.ffprobe_path.string(), "-v", "error", "-select_streams", stream, "-show_entries",
       "stream_side_data=displaymatrix", "-of", "json", file.string()},
      report, request.scratch_dir / "display-matrix.log", request.cancellation);
  if (!outcome.succeeded) {
    return std::nullopt;
  }
  std::ifstream input(report, std::ios::binary);
  const nlohmann::json value = nlohmann::json::parse(input, nullptr, false);
  if (!value.is_object() || !value.contains("streams") || !value["streams"].is_array() ||
      value["streams"].size() != 1) {
    return std::nullopt;
  }
  std::string matrix;
  const nlohmann::json& found = value["streams"][0];
  if (found.contains("side_data_list") && found["side_data_list"].is_array()) {
    for (const nlohmann::json& side_data : found["side_data_list"]) {
      if (side_data.contains("displaymatrix") && side_data["displaymatrix"].is_string()) {
        matrix += side_data["displaymatrix"].get<std::string>();
      }
    }
  }
  return matrix;
}

// Cuts the source into `plan`'s pieces in the first container that holds
// them exactly. The pieces' paths, or nullopt with `why` set.
std::optional<std::vector<std::filesystem::path>> cut_pieces(
    const SplitResizeRequest& request, const PacketTable& source, const PiecePlan& plan,
    const std::filesystem::path& directory, std::string& why) {
  const std::string stream = std::to_string(request.video.index);
  for (const PieceContainer& container : kPieceContainers) {
    // What an earlier container's cut left.
    std::vector<std::filesystem::path> left;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
      left.push_back(entry.path());
    }
    for (const std::filesystem::path& file : left) {
      std::filesystem::remove(file);
    }
    const std::string label = std::string(container.muxer) + ": ";
    const std::filesystem::path pattern =
        directory / ("piece%05d." + std::string(container.extension));
    const CommandOutcome cut = run_cancellable_command(
        piece_cut_command(request.ffmpeg_path, request.source_path, request.video.index,
                          plan.first_packets, container, pattern),
        request.scratch_dir / "cut.out", request.scratch_dir / "cut.log", request.cancellation);
    if (cut.cancelled || cancelled(request.cancellation)) {
      why = "cancelled";
      return std::nullopt;
    }
    if (!cut.succeeded) {
      why += (why.empty() ? "" : "; ") + label + "ffmpeg: " + cut.diagnostics;
      continue;
    }
    // Each piece is read by its own ffprobe, as many at once as this Mac has
    // cores: reading them one after another took longer than cutting them.
    const std::size_t count = plan.first_packets.size();
    std::vector<std::filesystem::path> pieces(count);
    std::vector<std::optional<std::string>> mismatches(count);
    std::atomic<std::size_t> next = 0;
    const auto check = [&]() {
      for (std::size_t i = next++; i < count; i = next++) {
        pieces[i] = directory / piece_name("piece", i, container.extension);
        std::string error;
        const std::optional<PacketTable> packets = probe_packets(
            request.ffprobe_path, pieces[i], "v:0", request.scratch_dir, request.cancellation,
            error);
        if (!packets) {
          mismatches[i] = "piece " + std::to_string(i) + ": " + error;
          continue;
        }
        const std::size_t end = i + 1 < count ? plan.first_packets[i + 1] : source.packets.size();
        if (std::optional<std::string> differs =
                piece_mismatch(source, plan.first_packets[i], end, *packets)) {
          mismatches[i] = "piece " + std::to_string(i) + ": " + *differs;
        }
      }
    };
    std::vector<std::thread> checkers;
    const std::size_t threads =
        std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, count);
    for (std::size_t t = 0; t < threads; ++t) {
      checkers.emplace_back(check);
    }
    for (std::thread& checker : checkers) {
      checker.join();
    }
    std::optional<std::string> mismatch;
    for (const std::optional<std::string>& found : mismatches) {
      if (found && !mismatch) {
        mismatch = found;
      }
    }
    if (cancelled(request.cancellation)) {
      why = "cancelled";
      return std::nullopt;
    }
    if (!mismatch && std::filesystem::exists(
                         directory / piece_name("piece", plan.first_packets.size(),
                                                container.extension))) {
      mismatch = "ffmpeg cut more pieces than planned";
    }
    if (!mismatch) {
      return pieces;
    }
    why += (why.empty() ? "" : "; ") + label + *mismatch;
  }
  return std::nullopt;
}

}  // namespace

PieceQueue::PieceQueue(std::vector<PieceJob> jobs) : jobs_(std::move(jobs)) {
  for (std::size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].index != i) {
      throw std::invalid_argument("piece jobs must be numbered in order from 0");
    }
    waiting_.insert(i);
  }
}

std::optional<PieceJob> PieceQueue::take() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] { return closed_ || done_.size() == jobs_.size() || !waiting_.empty(); });
  if (closed_ || waiting_.empty()) {
    return std::nullopt;
  }
  const std::size_t index = *waiting_.begin();
  waiting_.erase(waiting_.begin());
  return jobs_[index];
}

void PieceQueue::encoded(std::size_t index) {
  {
    const std::lock_guard lock(mutex_);
    if (!done_.insert(index).second) {
      return;
    }
  }
  std::error_code ignored;
  std::filesystem::remove(jobs_.at(index).piece, ignored);
  changed_.notify_all();
}

void PieceQueue::give_back(std::size_t index) {
  {
    const std::lock_guard lock(mutex_);
    if (!done_.contains(index)) {
      waiting_.insert(index);
    }
  }
  changed_.notify_all();
}

void PieceQueue::close() {
  {
    const std::lock_guard lock(mutex_);
    closed_ = true;
  }
  changed_.notify_all();
}

bool PieceQueue::all_encoded() const {
  const std::lock_guard lock(mutex_);
  return done_.size() == jobs_.size();
}

bool PieceQueue::closed() const {
  const std::lock_guard lock(mutex_);
  return closed_;
}

void PieceQueue::wait() const {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] { return closed_ || done_.size() == jobs_.size(); });
}

SplitResizeOutcome split_resize(const SplitResizeRequest& request) {
  const auto report = [&request](const std::string& line) {
    if (request.report) {
      request.report(line);
    }
  };
  std::string error;
  const std::optional<PacketTable> source =
      probe_packets(request.ffprobe_path, request.source_path, std::to_string(request.video.index),
                    request.scratch_dir, request.cancellation, error);
  if (cancelled(request.cancellation)) {
    return outcome(Status::cancelled, "cancelled");
  }
  if (!source) {
    return outcome(Status::not_split, "cannot read the video's packets: " + error);
  }
  const PiecePlanResult planned = plan_pieces(*source);
  if (!planned.plan) {
    return outcome(Status::not_split, planned.why_not);
  }
  const PiecePlan& plan = *planned.plan;
  const std::optional<std::int64_t> first_frame_offset_us =
      probe_start_offset_us(request.ffprobe_path, request.source_path,
                            std::to_string(request.video.index), request.scratch_dir,
                            request.cancellation, error);
  const std::optional<std::string> source_matrix =
      display_matrix(request, request.source_path, std::to_string(request.video.index));
  if (cancelled(request.cancellation)) {
    return outcome(Status::cancelled, "cancelled");
  }
  if (!first_frame_offset_us || !source_matrix) {
    return outcome(Status::not_split, "cannot read the video's start time or display matrix");
  }

  std::optional<PiecesDirectory> directory;
  try {
    directory.emplace(request.output_path);
  } catch (const std::exception& failure) {
    return outcome(Status::failed, failure.what());
  }
  std::string why;
  const std::optional<std::vector<std::filesystem::path>> pieces =
      cut_pieces(request, *source, plan, directory->path(), why);
  if (cancelled(request.cancellation)) {
    return outcome(Status::cancelled, "cancelled");
  }
  if (!pieces) {
    return outcome(Status::failed, "the video cannot be cut exactly: " + why);
  }
  report("resizing in " + std::to_string(pieces->size()) + " pieces" +
         (request.remote ? ", on this Mac and its paired workers" : ""));

  std::vector<PieceJob> jobs;
  std::vector<std::string> encoded_names;
  for (std::size_t i = 0; i < pieces->size(); ++i) {
    encoded_names.push_back(piece_name("encoded", i, "mov"));
    jobs.push_back(PieceJob{.index = i,
                            .piece = (*pieces)[i],
                            .encoded = directory->path() / encoded_names.back()});
  }
  const PieceEncoding encoding = piece_encoding(request.geometry, request.video.timing.timebase);
  PieceQueue queue(jobs);
  std::atomic<std::size_t> local_pieces = 0;
  std::string local_failure;
  std::thread local([&] {
    while (const std::optional<PieceJob> job = queue.take()) {
      const CommandOutcome encoded = run_cancellable_command(
          piece_encode_command(request.ffmpeg_path, job->piece, encoding, job->encoded),
          request.scratch_dir / "encode.out", request.scratch_dir / "encode.log",
          request.cancellation);
      if (encoded.succeeded) {
        ++local_pieces;
        queue.encoded(job->index);
        continue;
      }
      if (!encoded.cancelled) {
        local_failure = "piece " + std::to_string(job->index) + ": ffmpeg mpeg4: " +
                        encoded.diagnostics;
      }
      queue.close();
    }
  });
  std::thread remote;
  if (request.remote) {
    remote = std::thread(
        [&] { request.remote->run(queue, encoding, report, request.cancellation); });
  }
  // A cancelled build stops the commands, which close the queue; a remote
  // Mac's piece is abandoned once the remote encoders see the cancellation.
  std::thread watch([&] {
    while (!queue.closed() && !queue.all_encoded()) {
      if (cancelled(request.cancellation)) {
        queue.close();
        return;
      }
      std::this_thread::sleep_for(kCommandCancellationPollInterval);
    }
  });
  queue.wait();
  local.join();
  if (remote.joinable()) {
    remote.join();
  }
  queue.close();
  watch.join();
  if (cancelled(request.cancellation)) {
    return outcome(Status::cancelled, "cancelled");
  }
  if (!queue.all_encoded()) {
    return outcome(Status::failed, local_failure.empty() ? "not every piece was encoded"
                                                         : local_failure);
  }

  // Each piece lasts from its first frame's source time to the next's.
  std::vector<std::int64_t> start_times_us;
  const std::int64_t first_pts = *source->packets[plan.first_packets.front()].pts;
  for (const std::size_t first : plan.first_packets) {
    start_times_us.push_back(microseconds(*source->packets[first].pts - first_pts,
                                          source->time_base));
  }
  const std::filesystem::path list = directory->path() / kConcatListName;
  {
    std::ofstream out(list, std::ios::binary | std::ios::trunc);
    out << piece_concat_list(encoded_names, start_times_us);
    if (!out) {
      return outcome(Status::failed, "cannot write " + list.string());
    }
  }
  const CommandOutcome joined = run_cancellable_command(
      piece_join_command(request.ffmpeg_path,
                         JoinInput{.concat_list = list,
                                   .source = request.source_path,
                                   .source_video_index = request.video.index,
                                   .first_frame_offset_us = *first_frame_offset_us,
                                   .rotation_degrees = request.video.rotation_degrees,
                                   .container = request.container},
                         request.output_path),
      request.scratch_dir / "join.out", request.scratch_dir / "join.log", request.cancellation);
  if (joined.cancelled || cancelled(request.cancellation)) {
    return outcome(Status::cancelled, "cancelled");
  }
  if (!joined.succeeded) {
    return outcome(Status::failed, "joining the pieces: ffmpeg: " + joined.diagnostics);
  }

  const std::optional<PacketTable> result = probe_packets(
      request.ffprobe_path, request.output_path, "v:0", request.scratch_dir, request.cancellation,
      error);
  if (!result) {
    return outcome(Status::failed, "cannot read the joined video's packets: " + error);
  }
  if (std::optional<std::string> mismatch = joined_mismatch(*source, *result)) {
    return outcome(Status::failed, *mismatch);
  }
  const std::optional<std::string> result_matrix =
      display_matrix(request, request.output_path, "v:0");
  if (!result_matrix || *result_matrix != *source_matrix) {
    return outcome(Status::failed, "the joined video's display matrix is not the source's");
  }
  return SplitResizeOutcome{.status = Status::written,
                            .why = {},
                            .pieces = pieces->size(),
                            .remote_pieces = pieces->size() - local_pieces.load()};
}

}  // namespace svp::builder::resize
