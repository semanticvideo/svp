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
                                         int width,
                                         int height,
                                         const std::string& purpose,
                                         bool keyframe) {
  const FrameKey key{timestamp_us, width, height};
  const auto it = key_to_index_.find(key);
  if (it != key_to_index_.end()) {
    auto& entry = entries_[it->second];
    entry.purposes.insert(purpose);
    if (keyframe)
      entry.keyframe = true;
    return entry.frame_id;
  }

  const std::size_t index = entries_.size();
  FrameCatalogEntry entry;
  entry.frame_id = make_frame_id(index);
  entry.frame_index = index;
  entry.timestamp_us = timestamp_us;
  entry.width = width;
  entry.height = height;
  entry.keyframe = keyframe;
  entry.purposes.insert(purpose);
  key_to_index_[key] = index;
  entries_.push_back(std::move(entry));
  return entries_.back().frame_id;
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
    std::int64_t timestamp_us, int width, int height) const {
  const FrameKey key{timestamp_us, width, height};
  const auto it = key_to_index_.find(key);
  if (it == key_to_index_.end())
    return std::nullopt;
  return entries_[it->second].frame_index;
}

const std::vector<FrameCatalogEntry>& FrameCatalog::entries() const {
  return entries_;
}

std::size_t FrameCatalog::size() const {
  return entries_.size();
}

}  // namespace svp::vision
