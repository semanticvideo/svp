#include "types.hpp"

#include <iomanip>
#include <sstream>

namespace svp::audio::sherpa_diarization_internal::word_assignment {

std::string speaker_id_for_index(int32_t speaker_index) {
  std::ostringstream sid;
  sid << "speaker_" << std::setw(4) << std::setfill('0') << (speaker_index + 1);
  return sid.str();
}

int32_t speaker_index_from_id(const std::string& speaker_id,
                              int32_t speaker_count) {
  if (speaker_id.rfind("speaker_", 0) != 0) return -1;
  try {
    const int32_t one_based = std::stoi(speaker_id.substr(8));
    const int32_t zero_based = one_based - 1;
    if (zero_based >= 0 && zero_based < speaker_count) {
      return zero_based;
    }
  } catch (...) {
    return -1;
  }
  return -1;
}

}  // namespace svp::audio::sherpa_diarization_internal::word_assignment
