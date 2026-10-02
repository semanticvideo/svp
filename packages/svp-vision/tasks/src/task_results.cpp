#include "task_results.hpp"

#include "svp/exec/output_digest.hpp"

#include <algorithm>
#include <stdexcept>

namespace svp::vision::tasks::detail {
namespace {

// TaskTypeRegistry::execute validates the result before run_task_attempt
// stamps the real attempt number and worker session over these.
constexpr std::uint64_t kUnstampedAttempt = 1;
constexpr std::string_view kUnstampedWorkerSession = "ws_unstamped";

svp::exec::TaskResult unstamped_result(const svp::exec::TaskSpec& spec) {
  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = kUnstampedAttempt;
  result.execution.worker_session_id = std::string(kUnstampedWorkerSession);
  return result;
}

std::vector<std::byte> to_bytes(const std::string& text) {
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](char character) { return static_cast<std::byte>(character); });
  return bytes;
}

}  // namespace

svp::exec::TaskResult failed_result(const svp::exec::TaskSpec& spec, std::string_view task_type,
                                    std::string code, std::string message, bool retryable) {
  svp::exec::TaskResult result = unstamped_result(spec);
  result.status = svp::exec::TaskStatus::failed;
  result.output_digest = svp::exec::compute_output_digest({});
  result.error = svp::exec::TaskError{.code = std::move(code),
                                      .message = std::string(task_type) + ": " +
                                                 std::move(message),
                                      .retryable = retryable};
  return result;
}

svp::exec::TaskResult succeeded_result(const svp::exec::TaskSpec& spec,
                                       const OutputWriter& write_output,
                                       const std::vector<nlohmann::json>& records,
                                       const std::vector<std::byte>& data,
                                       std::string_view records_role,
                                       std::string_view data_role,
                                       nlohmann::json diagnostics) {
  std::string jsonl;
  for (const nlohmann::json& record : records) {
    jsonl += record.dump();
    jsonl += '\n';
  }
  svp::exec::TaskResult result = unstamped_result(spec);
  result.status = svp::exec::TaskStatus::succeeded;
  result.outputs = {
      write_output(to_bytes(jsonl), std::string(kRecordsMediaType), std::string(records_role)),
      write_output(data, std::string(kDataMediaType), std::string(data_role)),
  };
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  result.diagnostics = std::move(diagnostics);
  return result;
}

RecordsAndData read_records_and_data(const svp::exec::TaskSpec& spec,
                                     const std::vector<svp::exec::ArtifactRef>& outputs,
                                     const std::vector<std::vector<std::byte>>& payloads,
                                     std::string_view records_role, std::string_view data_role) {
  if (outputs.size() != 2 || payloads.size() != 2 || outputs[0].role != records_role ||
      outputs[1].role != data_role) {
    throw std::invalid_argument(spec.task_id + ": expected outputs `" +
                                std::string(records_role) + "` and `" +
                                std::string(data_role) + "`");
  }
  RecordsAndData read;
  const std::vector<std::byte>& text = payloads[0];
  std::size_t start = 0;
  while (start < text.size()) {
    const auto newline = std::find(text.begin() + static_cast<std::ptrdiff_t>(start), text.end(),
                                   std::byte{'\n'});
    if (newline == text.end()) {
      throw std::invalid_argument(spec.task_id + ": records do not end with a newline");
    }
    const std::size_t end = static_cast<std::size_t>(newline - text.begin());
    const std::string_view line(reinterpret_cast<const char*>(text.data()) + start, end - start);
    nlohmann::json record = nlohmann::json::parse(line.begin(), line.end(), nullptr, false);
    if (record.is_discarded() || !record.is_object()) {
      throw std::invalid_argument(spec.task_id + ": record is not a JSON object");
    }
    read.records.push_back(std::move(record));
    start = end + 1;
  }
  read.data = std::span<const std::byte>(payloads[1]);
  return read;
}

nlohmann::json append_data(std::vector<std::byte>& data, std::span<const std::byte> bytes) {
  nlohmann::json range = {{"data_bytes", bytes.size()}, {"data_offset", data.size()}};
  data.insert(data.end(), bytes.begin(), bytes.end());
  return range;
}

std::span<const std::byte> record_data(const nlohmann::json& record,
                                       std::span<const std::byte> data,
                                       const std::string& where) {
  const auto offset_it = record.find("data_offset");
  const auto bytes_it = record.find("data_bytes");
  if (offset_it == record.end() || bytes_it == record.end() ||
      !offset_it->is_number_unsigned() || !bytes_it->is_number_unsigned()) {
    throw std::invalid_argument(where + ": record has no data range");
  }
  const std::uint64_t offset = offset_it->get<std::uint64_t>();
  const std::uint64_t bytes = bytes_it->get<std::uint64_t>();
  if (offset > data.size() || bytes > data.size() - offset) {
    throw std::invalid_argument(where + ": data range is outside the task's data");
  }
  return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(bytes));
}

}  // namespace svp::vision::tasks::detail
