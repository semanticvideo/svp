#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace svp::vision {

struct FrameCatalogEntry {
  std::string frame_id;
  std::size_t frame_index = 0;
  std::int64_t timestamp_us = 0;
  int width = 0;
  int height = 0;
  bool keyframe = false;
  std::set<std::string> purposes;
};

class FrameCatalog {
public:
  std::string register_frame(std::int64_t timestamp_us,
                             int width,
                             int height,
                             const std::string& purpose,
                             bool keyframe = false);

  std::optional<std::size_t> get_frame_index(const std::string& frame_id) const;

  std::optional<std::size_t> get_frame_index(std::int64_t timestamp_us,
                                             int width,
                                             int height) const;

  const std::vector<FrameCatalogEntry>& entries() const;

  std::size_t size() const;

private:
  struct FrameKey {
    std::int64_t timestamp_us;
    int width;
    int height;
    bool operator<(const FrameKey& other) const {
      if (timestamp_us != other.timestamp_us)
        return timestamp_us < other.timestamp_us;
      if (width != other.width)
        return width < other.width;
      return height < other.height;
    }
  };

  std::map<FrameKey, std::size_t> key_to_index_;
  std::vector<FrameCatalogEntry> entries_;

  static std::string make_frame_id(std::size_t index);
};

}  // namespace svp::vision
