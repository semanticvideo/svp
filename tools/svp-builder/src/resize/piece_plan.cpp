#include "resize/piece_plan.hpp"

#include <algorithm>
#include <limits>

namespace svp::builder::resize {

PiecePlanResult plan_pieces(const PacketTable& table) {
  const std::vector<SourcePacket>& packets = table.packets;
  if (packets.empty()) {
    return {.why_not = "the video has no packets"};
  }
  if (table.time_base.numerator <= 0 || table.time_base.denominator <= 0) {
    return {.why_not = "the video has no time base"};
  }
  for (const SourcePacket& packet : packets) {
    if (!packet.pts) {
      return {.why_not = "the video's packets have no presentation times"};
    }
    if (packet.hidden) {
      return {.why_not = "the video has packets that are never shown or are corrupt"};
    }
  }

  // later_min[i]: the earliest presentation time of packets i.. (file order).
  const std::size_t count = packets.size();
  std::vector<std::int64_t> later_min(count + 1, std::numeric_limits<std::int64_t>::max());
  for (std::size_t i = count; i-- > 0;) {
    later_min[i] = std::min(later_min[i + 1], *packets[i].pts);
  }
  const auto clean_cut = [&](std::size_t i, std::int64_t earlier_max, bool any_earlier) {
    const std::int64_t pts = *packets[i].pts;
    return packets[i].keyframe && (!any_earlier || earlier_max < pts) &&
           later_min[i + 1] > pts;
  };
  if (!clean_cut(0, 0, false) || later_min[0] != *packets[0].pts) {
    return {.why_not = "the video does not start on a keyframe that decodes on its own"};
  }

  // A cut is due once (pts - first) * time_base >= next_cut * kPieceSeconds,
  // compared in whole time-base units: (pts - first) * num >= next * s * den.
  const std::int64_t first = *packets[0].pts;
  PiecePlan plan{.first_packets = {0}};
  __int128 next_cut = 1;
  std::int64_t earlier_max = first;
  for (std::size_t i = 1; i < count; ++i) {
    const std::int64_t pts = *packets[i].pts;
    const __int128 elapsed = static_cast<__int128>(pts - first) * table.time_base.numerator;
    if (elapsed >= next_cut * kPieceSeconds * table.time_base.denominator &&
        clean_cut(i, earlier_max, true)) {
      plan.first_packets.push_back(i);
      while (elapsed >= next_cut * kPieceSeconds * table.time_base.denominator) {
        ++next_cut;
      }
    }
    earlier_max = std::max(earlier_max, pts);
  }
  if (plan.first_packets.size() < 2) {
    return {.why_not = "the video has no keyframe to cut at " + std::to_string(kPieceSeconds) +
                       " s or more after its start"};
  }
  return {.plan = std::move(plan)};
}

}  // namespace svp::builder::resize
