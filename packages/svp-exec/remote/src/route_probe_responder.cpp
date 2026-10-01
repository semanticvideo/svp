#include "route_probe_responder.hpp"

#include "route_probe_wire.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <algorithm>
#include <vector>

namespace svp::exec::remote::detail {
namespace {

bool read_exactly(RemoteStream& stream, std::span<std::byte> buffer) {
  std::size_t filled = 0;
  while (filled < buffer.size()) {
    const std::size_t count = stream.read_some(buffer.subspan(filled));
    if (count == 0) {
      return false;
    }
    filled += count;
  }
  return true;
}

}  // namespace

bool answer_route_probe(RemoteStream& stream) {
  try {
    std::array<std::byte, kRouteProbeHeaderBytes> header{};
    if (!read_exactly(stream, header)) {
      return false;
    }
    const std::uint64_t length = decode_probe_length(header);
    if (length == 0 || length > kMaxRouteProbeBytes) {
      return false;
    }
    std::vector<std::byte> chunk(kRouteProbeChunkBytes);
    std::uint64_t received = 0;
    while (received < length) {
      const std::size_t want =
          static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), length - received));
      const std::size_t count = stream.read_some(std::span(chunk.data(), want));
      if (count == 0) {
        return false;
      }
      received += count;
    }
    for (std::uint64_t sent = 0; sent < length; sent += chunk.size()) {
      const std::size_t count =
          static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), length - sent));
      stream.write_all(std::span<const std::byte>(chunk.data(), count));
    }
    // The coordinator closes once it has everything; until then cancelling
    // could cut off bytes still in flight.
    std::byte ignored{};
    (void)stream.read_some(std::span(&ignored, 1));
    return true;
  } catch (const ExecError&) {
    return false;
  }
}

}  // namespace svp::exec::remote::detail
