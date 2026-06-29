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
  bool keyframe = false;
  std::set<std::string> purposes;
};

class FrameCatalog {
public:
  std::string register_frame(std::int64_t timestamp_us,
                             const std::string& purpose,
                             bool keyframe = false);

  std::optional<std::size_t> get_frame_index(const std::string& frame_id) const;

  std::optional<std::size_t> get_frame_index(std::int64_t timestamp_us) const;

  const std::vector<FrameCatalogEntry>& entries() const;

  std::size_t size() const;

private:
  std::map<std::int64_t, std::size_t> timestamp_to_index_;
  std::vector<FrameCatalogEntry> entries_;

  static std::string make_frame_id(std::size_t index);
};

}  // namespace svp::vision
