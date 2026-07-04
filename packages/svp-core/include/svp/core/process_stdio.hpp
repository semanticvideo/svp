#pragma once

#include <mutex>

namespace svp::core {

std::recursive_mutex& process_stdio_suppression_mutex();

}  // namespace svp::core
