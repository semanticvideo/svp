#pragma once

// TaskResult construction shared by the dispatched vision task types
// (ocr.crop_batch, embed.text_batch, embed.keyframe_batch,
// depth.frame_batch). Each returns two outputs: a JSONL of one record per
// item, in parameter order, and the items' binary data (crop images,
// vectors, depth fields) concatenated, each record naming its byte range.

#include "svp/exec/artifact_ref.hpp"
#include "svp/exec/task_result.hpp"
#include "svp/exec/task_spec.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace svp::vision::tasks::detail {

// Stores output bytes in the runtime's artifact store and returns their ref.
using OutputWriter = std::function<svp::exec::ArtifactRef(
    std::span<const std::byte> bytes, std::string media_type, std::string role)>;

inline constexpr std::string_view kRecordsMediaType = "application/x-ndjson";
inline constexpr std::string_view kDataMediaType = "application/octet-stream";

// A failed attempt of a `task_type` task (no outputs).
[[nodiscard]] svp::exec::TaskResult failed_result(const svp::exec::TaskSpec& spec,
                                                  std::string_view task_type, std::string code,
                                                  std::string message, bool retryable);

// A succeeded attempt with the records (one JSON object per item, joined as
// JSONL) and the data bytes, under `records_role` and `data_role`.
[[nodiscard]] svp::exec::TaskResult succeeded_result(
    const svp::exec::TaskSpec& spec, const OutputWriter& write_output,
    const std::vector<nlohmann::json>& records, const std::vector<std::byte>& data,
    std::string_view records_role, std::string_view data_role,
    nlohmann::json diagnostics);

// The records and data of a committed result: checks there are exactly the
// two outputs with these roles, one parseable JSON object per line. Throws
// std::invalid_argument naming the task otherwise.
struct RecordsAndData {
  std::vector<nlohmann::json> records;
  std::span<const std::byte> data;
};
[[nodiscard]] RecordsAndData read_records_and_data(
    const svp::exec::TaskSpec& spec, const std::vector<svp::exec::ArtifactRef>& outputs,
    const std::vector<std::vector<std::byte>>& payloads, std::string_view records_role,
    std::string_view data_role);

// Appends `bytes` to `data` and returns {"data_offset","data_bytes"} for a
// record.
nlohmann::json append_data(std::vector<std::byte>& data, std::span<const std::byte> bytes);

// The byte range a record names, checked against `data`. Throws
// std::invalid_argument for a range outside it.
[[nodiscard]] std::span<const std::byte> record_data(const nlohmann::json& record,
                                                     std::span<const std::byte> data,
                                                     const std::string& where);

}  // namespace svp::vision::tasks::detail
