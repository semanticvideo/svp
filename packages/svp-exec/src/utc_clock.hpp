#pragma once

#include <string>

namespace svp::exec::detail {

// Current UTC time as "YYYY-MM-DDTHH:MM:SS.mmmZ". Millisecond precision
// orders journal events within one second without implying more accuracy
// than the system clock guarantees.
[[nodiscard]] std::string utc_now_millis();

}  // namespace svp::exec::detail
