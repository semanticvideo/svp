#pragma once

// Bytes for a VisualEntityWindowOutcome (visual_entity_window.hpp), so a
// window computed in another process or on another Mac reaches the fold
// exactly as computed: decoding returns an outcome equal, field for field, to
// the one encoded.
//
// The encoding is CBOR (RFC 8949) of one map, written by nlohmann::json's
// to_cbor: floating-point values keep their exact bits (non-finite values
// included), map keys are sorted, and equal outcomes encode to equal bytes.
// Region masks travel as SVP RLE (spec §14.3, the package's own mask
// encoding) and appearance embeddings as little-endian IEEE-754 binary32.

#include "svp/vision/visual_entity_window.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace svp::vision {

// Format version of the encoding below; it changes with any change to the
// layout or meaning of a field.
inline constexpr std::uint64_t kVisualEntityWindowCodecVersion = 1;

class VisualEntityWindowCodecError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Throws VisualEntityWindowCodecError for a region mask whose pixels are not
// all 0 or 1 or do not cover width x height.
[[nodiscard]] std::vector<std::uint8_t> encode_visual_entity_window_outcome(
    const VisualEntityWindowOutcome& outcome);

// What a reducer needs before it folds: a window's status and the runtimes
// it ran with, read without decoding its regions and masks.
struct VisualEntityWindowHeader {
  VisualEntityWindowStatus status = VisualEntityWindowStatus::decode_failed;
  VisualEntityWindowRuntimeStatus runtime_status;
};

// Throws VisualEntityWindowCodecError for bytes that are not an encoding of
// this version.
[[nodiscard]] VisualEntityWindowHeader peek_visual_entity_window_header(
    std::span<const std::uint8_t> bytes);

// Strict inverse. Throws VisualEntityWindowCodecError for bytes that are not
// a well-formed encoding of this version.
[[nodiscard]] VisualEntityWindowOutcome decode_visual_entity_window_outcome(
    std::span<const std::uint8_t> bytes);

}  // namespace svp::vision
