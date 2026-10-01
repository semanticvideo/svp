#include "engine/frame_catalog_delta.hpp"

#include <string>

namespace svp::builder::engine {

nlohmann::json frame_catalog_delta(const svp::vision::FrameCatalog& after) {
  nlohmann::json delta = nlohmann::json::array();
  for (const svp::vision::FrameCatalogEntry& entry : after.entries()) {
    delta.push_back({{"keyframe", entry.keyframe},
                     {"purposes", entry.purposes},
                     {"timestamp_us", entry.timestamp_us}});
  }
  return delta;
}

void apply_frame_catalog_delta(svp::vision::FrameCatalog& catalog,
                               const nlohmann::json& delta) {
  for (const nlohmann::json& frame : delta) {
    const auto timestamp_us = frame.at("timestamp_us").get<std::int64_t>();
    const bool keyframe = frame.at("keyframe").get<bool>();
    for (const nlohmann::json& purpose : frame.at("purposes")) {
      static_cast<void>(
          catalog.register_frame(timestamp_us, purpose.get<std::string>(), keyframe));
    }
  }
}

}  // namespace svp::builder::engine
