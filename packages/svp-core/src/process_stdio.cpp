#include "svp/core/process_stdio.hpp"

namespace svp::core {

std::recursive_mutex& process_stdio_suppression_mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

}  // namespace svp::core
