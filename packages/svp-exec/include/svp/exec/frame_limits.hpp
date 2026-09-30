#pragma once

#include <cstddef>
#include <cstdint>

namespace svp::exec {

// Plan §4.3: every frame starts with a 4-byte big-endian header length.
inline constexpr std::size_t kFrameLengthPrefixBytes = 4;

// Frame headers are parsed whole into a JSON DOM before any field is checked,
// so their size bounds the receiver's parse memory. Headers carry control
// records only: a TaskSpec with its plan-computed parameters (an OCR frame
// batch's sample indices and timestamps are a few KiB) or a TaskResult with
// its diagnostics. 4 MiB is roughly a thousand times that while keeping a
// hostile or corrupt header's DOM far below a worker's memory budget; bulk
// data belongs in payloads.
inline constexpr std::uint32_t kDefaultMaxFrameHeaderBytes = 4U * 1024U * 1024U;

// Each payload is buffered whole and its BLAKE3 verified before any byte is
// used (plan §4.3). Blobs larger than this travel as several BLOB_PUT chunks
// (plan §4.3, "chunked, each chunk and the whole blob verified"); the 565 MB
// Gator source becomes nine chunks. 64 MiB keeps per-chunk framing overhead
// negligible while bounding what a single payload can make a receiver hold.
inline constexpr std::uint64_t kDefaultMaxFramePayloadBytes =
    64ULL * 1024ULL * 1024ULL;

// A frame is released only after every payload verifies, so the payload sum
// bounds one frame's buffered memory. Four maximum-size payloads (256 MiB)
// let a RESULT carry several large outputs while a worker receiving frames
// stays well inside the memory it reserves for running tasks (plan §3.5).
inline constexpr std::uint64_t kDefaultMaxFrameTotalPayloadBytes =
    4ULL * kDefaultMaxFramePayloadBytes;

// Limits applied by both the encoder (refuse to produce) and the decoder
// (refuse to accept). Tests and transports may tighten them; loosening them
// is a protocol decision.
struct FrameLimits {
  std::uint32_t max_header_bytes = kDefaultMaxFrameHeaderBytes;
  std::uint64_t max_payload_bytes = kDefaultMaxFramePayloadBytes;
  std::uint64_t max_total_payload_bytes = kDefaultMaxFrameTotalPayloadBytes;
};

}  // namespace svp::exec
