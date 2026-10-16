#include "resize/source_packets.hpp"

#include "resize/cancellable_command.hpp"

#include <charconv>
#include <fstream>
#include <iterator>

namespace svp::builder::resize {
namespace {

constexpr std::string_view kPacketSection = "packet";
constexpr std::string_view kStreamSection = "stream";
constexpr std::string_view kNoValue = "N/A";

std::vector<std::string_view> fields_of(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    fields.push_back(line.substr(start, comma == std::string_view::npos ? comma : comma - start));
    if (comma == std::string_view::npos) {
      return fields;
    }
    start = comma + 1;
  }
}

std::optional<std::int64_t> integer_of(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::optional<PacketTable> parse_packet_report(std::string_view csv) {
  PacketTable table;
  bool time_base_seen = false;
  std::size_t start = 0;
  while (start < csv.size()) {
    std::size_t end = csv.find('\n', start);
    if (end == std::string_view::npos) {
      end = csv.size();
    }
    std::string_view line = csv.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> fields = fields_of(line);
    if (fields.front() == kPacketSection) {
      // pts, size, flags: ffprobe prints a section's fields in its own order,
      // then any side data it has.
      if (fields.size() < 4) {
        return std::nullopt;
      }
      SourcePacket packet;
      if (fields[1] != kNoValue) {
        packet.pts = integer_of(fields[1]);
        if (!packet.pts) {
          return std::nullopt;
        }
      }
      const std::optional<std::int64_t> size = integer_of(fields[2]);
      if (!size || *size < 0) {
        return std::nullopt;
      }
      packet.size = *size;
      const std::string_view flags = fields[3];
      packet.keyframe = flags.find('K') != std::string_view::npos;
      packet.hidden = flags.find('D') != std::string_view::npos ||
                      flags.find('C') != std::string_view::npos;
      table.packets.push_back(packet);
    } else if (fields.front() == kStreamSection) {
      if (fields.size() < 2 || time_base_seen) {
        return std::nullopt;
      }
      const std::optional<svp::media::Rational> time_base = svp::media::parse_rational(fields[1]);
      if (!time_base || !svp::media::is_valid(*time_base) || time_base->numerator <= 0) {
        return std::nullopt;
      }
      table.time_base = svp::media::normalize(*time_base);
      time_base_seen = true;
    } else {
      return std::nullopt;
    }
  }
  if (!time_base_seen) {
    return std::nullopt;
  }
  return table;
}

std::optional<PacketTable> probe_packets(const std::filesystem::path& ffprobe_path,
                                         const std::filesystem::path& file,
                                         const std::string& stream,
                                         const std::filesystem::path& scratch_dir,
                                         const svp::exec::CancellationToken* cancellation,
                                         std::string& error) {
  // Named after the file, so several files can be read at once.
  const std::string name = file.filename().string();
  const std::filesystem::path report = scratch_dir / (name + ".packets.csv");
  const CommandOutcome outcome = run_cancellable_command(
      {ffprobe_path.string(), "-v", "error", "-select_streams", stream, "-show_entries",
       "packet=pts,size,flags:stream=time_base", "-of", "csv", file.string()},
      report, scratch_dir / (name + ".packets.log"), cancellation);
  if (!outcome.succeeded) {
    error = outcome.cancelled ? "cancelled" : outcome.diagnostics;
    return std::nullopt;
  }
  std::ifstream input(report, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  std::error_code ignored;
  std::filesystem::remove(report, ignored);
  std::optional<PacketTable> table = parse_packet_report(text);
  if (!table) {
    error = "ffprobe's packet report of " + file.filename().string() + " cannot be read";
  }
  return table;
}

}  // namespace svp::builder::resize
