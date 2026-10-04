#pragma once

// StreamDeadline lives with the transport (svp/exec/remote/stream_deadline.hpp)
// so the worker agent bounds its sessions the same way.

#include "svp/exec/remote/stream_deadline.hpp"

namespace svp::builder::workers {

using svp::exec::remote::StreamDeadline;

}  // namespace svp::builder::workers
