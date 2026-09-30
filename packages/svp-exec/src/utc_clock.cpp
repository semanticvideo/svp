#include "utc_clock.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace svp::exec::detail {

std::string utc_now_millis() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t seconds = system_clock::to_time_t(now);
  constexpr long long kMillisecondsPerSecond = 1000;
  const auto millis =
      duration_cast<milliseconds>(now.time_since_epoch()).count() % kMillisecondsPerSecond;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char buffer[sizeof("YYYY-MM-DDTHH:MM:SS.mmmZ")];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                static_cast<int>(millis));
  return buffer;
}

}  // namespace svp::exec::detail
