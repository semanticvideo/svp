#include "runtime_tools_json.hpp"

#include "svp/audio/sherpa_diarization.hpp"

#include <string>

namespace svp::builder {
namespace {

nlohmann::json tool_json(const RuntimeToolChoice& choice) {
  nlohmann::json value = {
      {"source", std::string(runtime_tool_source_name(choice.source))},
      {"path", choice.path},
  };
  if (!choice.blake3.empty()) value["blake3"] = choice.blake3;
  return value;
}

nlohmann::json sherpa_json(const RuntimeToolSelection& selection) {
  const svp::audio::SherpaLibSource source = svp::audio::sherpa_lib_source_used();
  if (source == svp::audio::SherpaLibSource::none) {
    return {{"loaded", false}};
  }
  nlohmann::json value = {
      {"loaded", true},
      {"source", std::string(svp::audio::sherpa_lib_source_name(source))},
      {"path", svp::audio::sherpa_lib_path_used()},
  };
  if (source == svp::audio::SherpaLibSource::bundled && selection.sherpa_bundled) {
    value["blake3"] = selection.sherpa_bundled->blake3;
  }
  return value;
}

}  // namespace

nlohmann::json runtime_tools_json(const RuntimeToolSelection& selection) {
  nlohmann::json value = nlohmann::json::object();
  if (selection.bundle_root) {
    value["bundle"] = {{"root", selection.bundle_root->string()}};
    if (!selection.runtime_id.empty()) {
      value["bundle"]["runtime_id"] = selection.runtime_id;
    }
  } else {
    value["bundle"] = nullptr;
  }
  if (selection.ffmpeg) value["ffmpeg"] = tool_json(*selection.ffmpeg);
  if (selection.ffprobe) value["ffprobe"] = tool_json(*selection.ffprobe);
  if (selection.uses_sherpa) value["sherpa_onnx"] = sherpa_json(selection);
  return value;
}

}  // namespace svp::builder
