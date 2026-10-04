#include "calibration/sherpa_library_naming.hpp"

#include "svp/audio/tasks/diarize_window.hpp"
#include "svp/builder/runtime_tools.hpp"

#include <exception>

namespace svp::builder::calibration {

std::optional<std::string> name_diarization_library(AudioCalibrationSetup& setup,
                                                    const std::filesystem::path& executable) {
  if (!setup.audio.diarization_model_ref) {
    return std::nullopt;
  }
  const auto skip = [&setup](const std::string& why) -> std::optional<std::string> {
    setup.audio.diarization_model_ref.reset();
    setup.sherpa_library.clear();
    return std::string(svp::audio::tasks::kDiarizeWindowTaskType) + ": " + why;
  };
  try {
    (void)offer_bundled_sherpa_library(locate_runtime_bundle(executable));
  } catch (const std::exception& error) {
    return skip(error.what());
  }
  const std::optional<std::string> library = svp::audio::tasks::expected_sherpa_library_identity();
  if (!library) {
    return skip("this Mac has no sherpa-onnx library to load");
  }
  setup.sherpa_library = *library;
  return std::nullopt;
}

}  // namespace svp::builder::calibration
