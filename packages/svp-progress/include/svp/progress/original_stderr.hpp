#pragma once

#include "svp/progress/fd_stream.hpp"

namespace svp::progress {

// Progress destination for command-line tools: the process's stderr as it was
// when first captured.
//
// Some stages silence noisy native libraries by temporarily pointing
// STDERR_FILENO at /dev/null while other threads keep emitting progress.
// Writing progress through this stable duplicate keeps every event visible.
// Call it once at process startup, before any worker thread exists, so the
// capture cannot race a temporary redirection; later calls return the same
// stream. The stream is intentionally never destroyed so it stays valid for
// progress emitted during static destruction.
FdStream& original_stderr();

}  // namespace svp::progress
