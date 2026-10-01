#include "transit_fault_writer.hpp"

#include "toy_tasks.hpp"

#include <string>
#include <vector>

namespace svp::exec::test {
namespace {

bool should_corrupt(const Frame& frame) {
  if (frame.type != MessageType::result || frame.payloads.empty()) {
    return false;
  }
  const nlohmann::json& diagnostics = frame.body.at("task_result").at("diagnostics");
  return diagnostics.contains(std::string(kToyCorruptInTransit));
}

}  // namespace

TransitFaultFrameWriter::TransitFaultFrameWriter(ByteStream& stream, FrameLimits limits)
    : stream_(stream), limits_(limits) {}

void TransitFaultFrameWriter::write(const Frame& frame) {
  std::vector<std::byte> bytes = encode_frame(frame, limits_);
  if (should_corrupt(frame)) {
    bytes.back() ^= std::byte{0x01};
  }
  const std::lock_guard lock(mutex_);
  stream_.write_all(bytes);
}

}  // namespace svp::exec::test
