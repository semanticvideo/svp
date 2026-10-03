#include "video_build_task.hpp"

#include "svp/builder/video_build_parameters.hpp"
#include "video_build_run.hpp"

#include "../cli/cli_run_telemetry.hpp"
#include "../workers/distributed_fleet.hpp"

#include "svp/builder/build_progress.hpp"
#include "svp/exec/cas_pin.hpp"
#include "svp/exec/cas_store.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/exec/worker/local_load.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"

#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace svp::builder::batch {
namespace {

static_assert(kVideoBuildTaskType == svp::exec::worker::kCoordinatingTaskType,
              "the agent counts coordinating Macs under the whole-video job's type");

constexpr std::string_view kPackageRecordMediaType = "application/json";
constexpr std::string_view kRunReportMediaType = "application/json";
constexpr std::string_view kModelLockFileName = "model-lock.json";
constexpr std::string_view kPinHolderSuffix = ".video-build";

// TaskTypeRegistry::execute validates the result before run_task_attempt
// stamps the real attempt number and worker session over these.
constexpr std::uint64_t kUnstampedAttempt = 1;
constexpr std::string_view kUnstampedWorkerSession = "ws_unstamped";

// The packages this session's jobs stored, pinned so cache eviction cannot
// remove one before the batch fetched it; released with the session.
class PackagePins {
 public:
  PackagePins(std::filesystem::path cas_root, std::string holder)
      : cas_root_(std::move(cas_root)), holder_(std::move(holder)) {}

  // Stores `file` in the worker cache and pins it.
  svp::exec::Blake3Digest store(const std::filesystem::path& file) {
    const std::lock_guard lock(mutex_);
    if (!store_) {
      svp::exec::CacheResult<svp::exec::CasStore> opened = svp::exec::CasStore::at(cas_root_);
      if (!opened) {
        throw std::runtime_error("cannot open the worker cache: " + opened.error().message);
      }
      store_.emplace(std::move(opened).value());
      svp::exec::CacheResult<svp::exec::CasPinSet> pins = store_->pin_set(holder_);
      if (!pins) {
        throw std::runtime_error("cannot pin in the worker cache: " + pins.error().message);
      }
      pins_.emplace(std::move(pins).value());
    }
    svp::exec::CacheResult<svp::exec::Blake3Digest> stored = store_->put_file(file);
    if (!stored) {
      throw std::runtime_error("cannot store the package in the worker cache: " +
                               stored.error().message);
    }
    const svp::exec::CacheStatus pinned = pins_->add(stored.value());
    if (!pinned) {
      throw std::runtime_error("cannot pin the package in the worker cache: " +
                               pinned.error().message);
    }
    return stored.value();
  }

 private:
  std::filesystem::path cas_root_;
  std::string holder_;
  std::mutex mutex_;
  std::optional<svp::exec::CasStore> store_;
  std::optional<svp::exec::CasPinSet> pins_;
};

// The worker's model view links each bundle directory into its store. A
// build's model-cache verification scans the cache without following
// directory links and accepts only regular files, so it would find no
// bundles there. The job's cache mirrors the view as real directories whose
// files are hard links to the store's files.
std::filesystem::path mirror_model_view(const std::filesystem::path& view,
                                        const std::filesystem::path& mirror) {
  std::filesystem::remove_all(mirror);
  std::filesystem::create_directories(mirror);
  for (const std::filesystem::directory_entry& bundle :
       std::filesystem::directory_iterator(view)) {
    const std::filesystem::path bundle_target = mirror / bundle.path().filename();
    std::filesystem::create_directories(bundle_target);
    for (auto it = std::filesystem::recursive_directory_iterator(
             bundle.path(), std::filesystem::directory_options::follow_directory_symlink);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      const std::filesystem::path target =
          bundle_target / std::filesystem::relative(it->path(), bundle.path());
      if (it->is_directory()) {
        std::filesystem::create_directories(target);
      } else {
        // Verification accepts only regular files, so link the store's
        // file itself (no copy); copy only across file systems.
        std::error_code linked;
        std::filesystem::create_hard_link(std::filesystem::canonical(it->path()), target, linked);
        if (linked) {
          std::filesystem::copy_file(std::filesystem::canonical(it->path()), target);
        }
      }
    }
  }
  return mirror;
}

svp::exec::TaskResult failed(const svp::exec::TaskSpec& spec, std::string_view code,
                             std::string message, bool retryable) {
  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = kUnstampedAttempt;
  result.execution.worker_session_id = std::string(kUnstampedWorkerSession);
  result.status = svp::exec::TaskStatus::failed;
  result.output_digest = svp::exec::compute_output_digest({});
  result.error = svp::exec::TaskError{.code = std::string(code),
                                      .message = std::string(kVideoBuildTaskType) + ": " +
                                                 std::move(message),
                                      .retryable = retryable};
  return result;
}

std::vector<std::byte> bytes_of(std::string_view text) {
  const auto* begin = reinterpret_cast<const std::byte*>(text.data());
  return std::vector<std::byte>(begin, begin + text.size());
}

std::string read_text(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  if (!in && !in.eof()) {
    throw std::runtime_error("cannot read " + file.string());
  }
  return text.str();
}

std::string package_file_name(const VideoBuildParameters& parameters) {
  const std::filesystem::path source(parameters.source_name);
  switch (parameters.output_format) {
    case VideoOutputFormat::svp:
      return source.stem().string() + ".svp";
    case VideoOutputFormat::svpi:
      return source.stem().string() + ".svpi";
    case VideoOutputFormat::embedded_svpi:
      return parameters.source_name;
  }
  return parameters.source_name;
}

svp::exec::TaskResult execute(const svp::exec::TaskSpec& spec,
                              const svp::exec::ResolvedInputs& inputs,
                              svp::exec::CasTaskArtifactAccess& artifacts,
                              const VideoBuildWorkerEnvironment& environment,
                              PackagePins& pins) {
  const VideoBuildParameters parameters = video_build_parameters_from_json(spec.parameters);

  // Could this Mac reproduce the batch Mac's build at all?
  const std::string ffmpeg_path =
      environment.tools.ffmpeg ? environment.tools.ffmpeg->path : std::string("ffmpeg");
  if (!parameters.ffmpeg_build.empty()) {
    const std::optional<std::string> here =
        svp::vision::tasks::cached_ffmpeg_build_identity(ffmpeg_path);
    if (!here || *here != parameters.ffmpeg_build) {
      return failed(spec, kVideoBuildUnavailableCode,
                    "this Mac's ffmpeg (" + ffmpeg_path + ", " + here.value_or("unknown") +
                        ") is not the batch Mac's build " + parameters.ffmpeg_build,
                    true);
    }
  }
  const std::filesystem::path job = environment.session_dir / ("video-" + spec.task_id);
  std::filesystem::path source;
  std::filesystem::path models;
  try {
    std::filesystem::create_directories(job / "source");
    std::filesystem::create_directories(job / "out");
    source = job / "source" / parameters.source_name;
    std::filesystem::remove(source);
    std::filesystem::create_symlink(inputs.at(std::string(kVideoBuildSourceInput)).path, source);
    models = mirror_model_view(environment.models->cache_for(spec), job / "models");
    const std::filesystem::path lock = models / kModelLockFileName;
    std::filesystem::remove(lock);
    std::filesystem::copy_file(inputs.at(std::string(kVideoBuildModelLockInput)).path, lock);
  } catch (const std::exception& error) {
    return failed(spec, kVideoBuildUnavailableCode,
                  std::string("cannot stage the job here: ") + error.what(), true);
  }

  const std::filesystem::path output = job / "out" / package_file_name(parameters);
  const std::filesystem::path report = job / "run-report.json";
  std::shared_ptr<DistributedExecution> distributed;
  if (parameters.distributed || parameters.require_workers > 0) {
    distributed = std::make_shared<svp::builder::workers::PairedWorkerFleet>(
        svp::builder::workers::DistributedFleetOptions{
            .require_workers = static_cast<std::size_t>(parameters.require_workers),
            .quiet = false});
  }
  VideoBuildRunResult run;
  {
    CliRunTelemetry telemetry("build", std::make_shared<NullBuildProgressSink>(),
                              parameters.run_report ? report : std::filesystem::path{},
                              environment.tools);
    run = run_video_build(VideoBuildRunOptions{
        .parameters = parameters,
        .source_path = source,
        .output_path = output,
        .staging_dir = job / "staging",
        .model_cache_dir = models,
        .ffprobe_path = environment.tools.ffprobe ? environment.tools.ffprobe->path
                                                  : std::string("ffprobe"),
        .ffmpeg_path = ffmpeg_path,
        .sherpa_lib_path = {},
        .runtime_tools = environment.tools,
        .overwrite_output = false,
        .use_parameter_thread_plan = true,
        .distributed = distributed,
        .progress_sink = telemetry.progress_sink(),
        .quiet = false,
        .verbose = false});
    telemetry.record_thread_plan(run.thread_plan);
    (void)telemetry.finish(run.success ? 0 : 1);
  }
  if (!run.success) {
    return failed(spec, kVideoBuildFailedCode, run.error_message, false);
  }

  svp::exec::TaskResult result;
  result.task_id = spec.task_id;
  result.attempt = kUnstampedAttempt;
  result.execution.worker_session_id = std::string(kUnstampedWorkerSession);
  result.status = svp::exec::TaskStatus::succeeded;
  try {
    const std::uint64_t bytes = std::filesystem::file_size(output);
    VideoBuildPackageRecord record{.file_name = output.filename().string(),
                                   .blake3 = pins.store(output),
                                   .bytes = bytes};
    // The cache holds it now; a package can be as large as its source, so
    // the scratch copy goes at once rather than with the session.
    std::error_code removed;
    std::filesystem::remove(output, removed);
    result.outputs.push_back(artifacts.put(bytes_of(encode_video_build_package_record(record)),
                                           std::string(kPackageRecordMediaType),
                                           std::string(kVideoBuildPackageRole)));
    if (parameters.run_report) {
      result.outputs.push_back(artifacts.put(bytes_of(read_text(report)),
                                             std::string(kRunReportMediaType),
                                             std::string(kVideoBuildRunReportRole)));
    }
  } catch (const std::exception& error) {
    return failed(spec, kVideoBuildUnavailableCode,
                  std::string("the package was built but cannot be kept here: ") + error.what(),
                  true);
  }
  result.output_digest = svp::exec::compute_output_digest(result.outputs);
  return result;
}

}  // namespace

void register_video_build_task(svp::exec::TaskTypeRegistry& registry,
                               svp::exec::CasTaskArtifactAccess& artifacts,
                               VideoBuildWorkerEnvironment environment) {
  auto pins = std::make_shared<PackagePins>(
      environment.cas_root, environment.worker_session_id + std::string(kPinHolderSuffix));
  registry.register_type(svp::exec::TaskTypeDefinition{
      .name = std::string(kVideoBuildTaskType),
      .version = kVideoBuildTaskTypeVersion,
      .validate_parameters = validate_video_build_parameters,
      .execute = [&artifacts, environment = std::move(environment), pins](
                     const svp::exec::TaskSpec& spec, const svp::exec::ResolvedInputs& inputs,
                     const svp::exec::CancellationToken&) {
        return execute(spec, inputs, artifacts, environment, *pins);
      }});
}

}  // namespace svp::builder::batch
