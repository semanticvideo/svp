#include "svp/progress/original_stderr.hpp"

#include <unistd.h>

namespace svp::progress {

FdStream& original_stderr() {
  // Leaked on purpose: see the header. Function-local static initialisation
  // is thread-safe, so the capture happens exactly once.
  static FdStream* const stream = new FdStream(STDERR_FILENO);
  return *stream;
}

}  // namespace svp::progress
