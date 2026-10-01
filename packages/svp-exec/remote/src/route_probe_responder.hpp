#pragma once

#include "svp/exec/remote/remote_stream.hpp"

namespace svp::exec::remote::detail {

// Worker side of a route probe (route_probe_wire.hpp) on an authenticated
// stream: sinks the coordinator's bytes, sends the same number back, and
// waits for the coordinator to close. Returns false for a malformed or
// oversize request or a stream that ended early.
bool answer_route_probe(RemoteStream& stream);

}  // namespace svp::exec::remote::detail
