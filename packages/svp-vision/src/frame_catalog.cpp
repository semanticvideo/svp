#include "svp/vision/frame_catalog.hpp"

#include <iomanip>
#include <sstream>

namespace svp::vision {

std::string FrameCatalog::make_frame_id(std::size_t index) {
  std::ostringstream oss;
  oss << "frame_" << std::setw(6) << std::setfill('0') << (index + 1);
  return oss.str();
}

std::string FrameCatalog::register_frame(std::int64_t timestamp_us,
                                         const std::string& purpose,
                                         bool keyframe) {
  const auto it = timestamp_to_index_.find(timestamp_us);
  if (it != timestamp_to_index_.end()) {
    auto& entry = entries_[it->second];
    entry.purposes.insert(purpose);
    if (keyframe)
      entry.keyframe = true;
    if (locked_to_plan_)
      entry.decoded = true;
    return entry.frame_id;
  }

  if (locked_to_plan_) {
    throw UnplannedFrameError(
        "frame at " + std::to_string(timestamp_us) + "us (purpose '" +
        purpose + "') is not in the locked frame plan");
  }

  const std::size_t index = entries_.size();
  FrameCatalogEntry entry;
  entry.frame_id = make_frame_id(index);
  entry.frame_index = index;
  entry.timestamp_us = timestamp_us;
  entry.keyframe = keyframe;
  entry.purposes.insert(purpose);
  entry.decoded = true;
  timestamp_to_index_[timestamp_us] = index;
  entries_.push_back(std::move(entry));
  return entries_.back().frame_id;
}

void FrameCatalog::lock_to_plan() {
  if (locked_to_plan_)
    return;
  locked_to_plan_ = true;
  for (auto& entry : entries_)
    entry.decoded = false;
}

bool FrameCatalog::locked_to_plan() const {
  return locked_to_plan_;
}

std::optional<std::size_t> FrameCatalog::get_frame_index(
    const std::string& frame_id) const {
  for (const auto& entry : entries_) {
    if (entry.frame_id == frame_id)
      return entry.frame_index;
  }
  return std::nullopt;
}

std::optional<std::size_t> FrameCatalog::get_frame_index(
    std::int64_t timestamp_us) const {
  const auto it = timestamp_to_index_.find(timestamp_us);
  if (it == timestamp_to_index_.end())
    return std::nullopt;
  return entries_[it->second].frame_index;
}

std::vector<FrameCatalogEntry> FrameCatalog::entries() const {
  std::vector<FrameCatalogEntry> decoded;
  decoded.reserve(entries_.size());
  for (const auto& entry : entries_) {
    if (entry.decoded)
      decoded.push_back(entry);
  }
  return decoded;
}

const std::vector<FrameCatalogEntry>& FrameCatalog::planned_entries() const {
  return entries_;
}

std::size_t FrameCatalog::size() const {
  std::size_t decoded = 0;
  for (const auto& entry : entries_) {
    if (entry.decoded)
      ++decoded;
  }
  return decoded;
}

}  // namespace svp::vision
