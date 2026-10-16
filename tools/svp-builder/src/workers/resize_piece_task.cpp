#include "resize_piece_task.hpp"

#include "resize/cancellable_command.hpp"
#include "resize/piece_task.hpp"

#include "svp/exec/output_digest.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace svp::builder::workers {
namespace {

using namespace svp::builder::resize;

constexpr std::string_view kRecordMediaType = "application/json";
constexpr std::string_view kPinHolderSuffix = ".resize-piece";

// TaskTypeRegistry::execute validates the result before run_task_attempt
// stamps the real attempt number and worker session over these.
constexpr std::uint64_t kUnstampedAttempt = 1;
constexpr std::string_view kUnstampedWorkerSession = "ws_unstamped";

svp::exec::TaskResult unstamped(const svp::exec::TaskSpec& spec) {
  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = kUnstampedAttempt;
  result.execution.worker_session_id = std::string(kUnstampedWorkerSession);
  return result;
}

svp::exec::TaskResult failed(const svp::exec::TaskSpec& spec, std::string_view code,
                             std::string message, bool retryable) {
  svp::exec::TaskResult result = unstamped(spec);
  result.status = svp::exec::TaskStatus::failed;
  result.output_digest = svp::exec::compute_output_digest({});
  result.error = svp::exec::TaskError{.code = std::string(code),
                                      .message = std::string(kResizePieceTaskType) + ": " +
                                                 std::move(message),
                                      .retryable = retryable};
  return result;
}

std::vector<std::byte> bytes_of(std::string_view text) {
  const auto* begin = reinterpret_cast<const std::byte*>(text.data());
  return std::vector<std::byte>(begin, begin + text.size());
}

svp::exec::TaskResult execute(const svp::exec::TaskSpec& spec,
                              const svp::exec::ResolvedInputs& inputs,
                              const svp::exec::CancellationToken& cancellation,
                              svp::exec::CasTaskArtifactAccess& artifacts,
                              const ResizePieceWorkerEnvironment& environment,
                              CasOutputPins& pins) {
  const ResizePieceParameters parameters = resize_piece_parameters_from_json(spec.parameters);
  const std::optional<std::string> here =
      svp::vision::tasks::cached_ffmpeg_build_identity(environment.ffmpeg_path.string());
  if (!here || *here != parameters.ffmpeg_build) {
    return failed(spec, kResizePieceUnavailableCode,
                  "this Mac's ffmpeg (" + environment.ffmpeg_path.string() + ", " +
                      here.value_or("unknown") + ") is not the coordinator's build " +
                      parameters.ffmpeg_build,
                  true);
  }
  const std::filesystem::path job = environment.session_dir / ("resize-" + spec.task_id);
  const std::filesystem::path encoded = job / "encoded.mov";
  try {
    std::filesystem::create_directories(job);
  } catch (const std::exception& error) {
    return failed(spec, kResizePieceUnavailableCode,
                  std::string("cannot stage the piece here: ") + error.what(), true);
  }
  const auto remove_job = [&job]() {
    std::error_code ignored;
    std::filesystem::remove_all(job, ignored);
  };
  const CommandOutcome outcome = run_cancellable_command(
      piece_encode_command(environment.ffmpeg_path,
                           inputs.at(std::string(kResizePieceInput)).path, parameters.encoding,
                           encoded),
      job / "encode.out", job / "encode.log", &cancellation);
  if (!outcome.succeeded) {
    remove_job();
    return failed(spec, kResizePieceFailedCode, "ffmpeg mpeg4: " + outcome.diagnostics,
                  outcome.cancelled || !outcome.started);
  }
  svp::exec::TaskResult result = unstamped(spec);
  result.status = svp::exec::TaskStatus::succeeded;
  try {
    const EncodedPieceRecord record{.blake3 = pins.store(encoded),
                                    .bytes = std::filesystem::file_size(encoded)};
    result.outputs.push_back(artifacts.put(bytes_of(encode_encoded_piece_record(record)),
                                           std::string(kRecordMediaType),
                                           std::string(kResizePieceEncodedRole)));
  } catch (const std::exception& error) {
    remove_job();
    return failed(spec, kResizePieceUnavailableCode,
                  std::string("the piece was encoded but cannot be kept here: ") + error.what(),
                  true);
  }
  // The cache holds it now.
  remove_job();
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  return result;
}

}  // namespace

void register_resize_piece_task(svp::exec::TaskTypeRegistry& registry,
                                svp::exec::CasTaskArtifactAccess& artifacts,
                                ResizePieceWorkerEnvironment environment) {
  auto pins = std::make_shared<CasOutputPins>(
      environment.cas_root, environment.worker_session_id + std::string(kPinHolderSuffix));
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kResizePieceTaskType),
      .version = kResizePieceTaskTypeVersion,
      .validate_parameters = validate_resize_piece_parameters,
      .execute = [&artifacts, environment = std::move(environment), pins](
                     const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                     const svp::exec::CancellationToken& cancellation) {
        return execute(spec, inputs, cancellation, artifacts, environment, *pins);
      }});
}

}  // namespace svp::builder::workers
