#pragma once

// One piece of a split resize (split_resize.hpp) encoded on a paired worker
// Mac: the task a coordinator assigns (resize.piece), its parameters, and
// the record its result carries. The worker runs piece_encode_command, the
// command this Mac runs for its own pieces, with the same ffmpeg build
// (checked), so a piece is the same bytes wherever it was encoded. The
// encoded piece stays in the worker's content-addressed cache, pinned, for
// the coordinator to fetch (BLOB_GET).

#include "resize/piece_commands.hpp"

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/blake3_digest.hpp"
#include "svp/exec/task_spec.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace svp::builder::resize {

inline constexpr std::string_view kResizePieceTaskType = "resize.piece";
inline constexpr std::uint64_t kResizePieceTaskTypeVersion = 1;
inline constexpr std::string_view kResizePieceInput = "piece";
inline constexpr std::string_view kResizePieceEncodedRole = "resize_piece_encoded";

// One piece at a time on each Mac: one encode already keeps a Mac busy
// (ffmpeg decodes on every core it has, and the encoder runs
// kEncoderThreads slices), and a second would only compete with it.
inline constexpr std::uint64_t kResizePieceSlotsPerMac = 1;

// Scheduling estimates the TaskSpec must carry (task_spec.hpp). The worker
// admits a piece by its one declared slot and by memory; there is no
// measured model of an encode's peak memory, so none is claimed (0), and
// one thread is the smallest estimate the spec accepts. No time estimate
// (0) leaves the lease's hard deadline at its floor (lease_policy.hpp), far
// longer than a kPieceSeconds piece takes on any Mac.
inline constexpr std::uint64_t kResizePieceEstimatedPeakRssMb = 0;
inline constexpr std::uint64_t kResizePieceEstimatedCpuThreads = 1;

// A piece that cannot be encoded on that Mac (its ffmpeg differs, its cache
// cannot keep the result): another Mac takes it. One that ffmpeg could not
// encode is reported with its own code; the coordinator encodes it itself.
inline constexpr std::string_view kResizePieceUnavailableCode = "resize_piece_unavailable";
inline constexpr std::string_view kResizePieceFailedCode = "resize_piece_failed";

struct ResizePieceParameters {
  PieceEncoding encoding;
  // The coordinator's ffmpeg build (ffmpeg_build_identity.hpp).
  std::string ffmpeg_build;
};

[[nodiscard]] nlohmann::json resize_piece_parameters_to_json(const ResizePieceParameters& value);
// Throws std::invalid_argument naming the field for anything outside the
// schema (unknown members included).
[[nodiscard]] ResizePieceParameters resize_piece_parameters_from_json(const nlohmann::json& value);
// nullopt when valid, otherwise why not (TaskParameterValidator).
[[nodiscard]] std::optional<std::string> validate_resize_piece_parameters(
    const nlohmann::json& value);

[[nodiscard]] svp::exec::TaskSpec make_resize_piece_spec(const std::string& build_session_id,
                                                         const std::string& task_id,
                                                         const svp::exec::ArtifactRef& piece,
                                                         const ResizePieceParameters& parameters);

// Where the encoded piece is: a blob of the worker's cache.
struct EncodedPieceRecord {
  svp::exec::Blake3Digest blake3{};
  std::uint64_t bytes = 0;
};

[[nodiscard]] std::string encode_encoded_piece_record(const EncodedPieceRecord& record);
// Throws std::invalid_argument when `text` is not one.
[[nodiscard]] EncodedPieceRecord decode_encoded_piece_record(std::string_view text);

}  // namespace svp::builder::resize
