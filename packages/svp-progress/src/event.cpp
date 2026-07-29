#include "svp/progress/renderer.hpp"

#include <stdexcept>

namespace svp::progress {

std::string_view event_kind_name(EventKind kind) {
  switch (kind) {
    case EventKind::started:
      return "stage_started";
    case EventKind::completed:
      return "stage_completed";
    case EventKind::failed:
      return "stage_failed";
    case EventKind::progress:
      return "stage_progress";
    case EventKind::warning:
      return "warning";
    case EventKind::artifact_written:
      return "artifact_written";
  }
  throw std::runtime_error("unknown progress event kind");
}

void NullSink::emit(const Event& /*event*/) {}

}  // namespace svp::progress
