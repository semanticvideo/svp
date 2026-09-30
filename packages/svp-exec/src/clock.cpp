#include "svp/exec/clock.hpp"

namespace svp::exec {

std::chrono::milliseconds SteadyClock::now() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
}

std::chrono::milliseconds ManualClock::now() const {
  return std::chrono::milliseconds(now_ms_.load());
}

void ManualClock::advance(std::chrono::milliseconds delta) {
  now_ms_.fetch_add(delta.count());
}

}  // namespace svp::exec
