#include "internal.hpp"

namespace svp::audio::transcript_writer_internal {

std::string asr_status_string(AsrStatus status) {
  switch (status) {
    case AsrStatus::planned: return "planned";
    case AsrStatus::blocked: return "blocked";
    case AsrStatus::ran: return "ran";
  }
  return "unknown";
}

}  // namespace svp::audio::transcript_writer_internal
