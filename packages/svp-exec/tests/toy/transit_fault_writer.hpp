#pragma once

// FrameWriter used by svp-exec-test-worker: encodes frames onto a ByteStream
// and corrupts the last payload byte of any RESULT whose diagnostics carry
// kToyCorruptInTransit, so the coordinator sees a payload hash mismatch
// exactly as it would from a faulty link. Every other frame passes through
// unchanged. Thread-safe.

#include "svp/exec/byte_stream.hpp"
#include "svp/exec/frame_stream.hpp"

#include <mutex>

namespace svp::exec::test {

class TransitFaultFrameWriter final : public FrameWriter {
 public:
  explicit TransitFaultFrameWriter(ByteStream& stream, FrameLimits limits = {});
  void write(const Frame& frame) override;

 private:
  ByteStream& stream_;
  FrameLimits limits_;
  std::mutex mutex_;
};

}  // namespace svp::exec::test
