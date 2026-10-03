#include "build_batch.hpp"

#include "existing_output_check.hpp"

#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

namespace svp::builder::batch {
namespace {

constexpr std::string_view kThisMac = "this Mac";
constexpr std::string_view kRunReportSuffix = ".run-report.json";
// Item ids name each video in the batch's whole-video jobs: "video_" and
// the video's position, zero-padded so ids sort in batch order.
constexpr std::string_view kItemIdPrefix = "video_";
constexpr int kItemIdDigits = 6;

std::string item_id(std::size_t index) {
  std::ostringstream id;
  id << kItemIdPrefix << std::setw(kItemIdDigits) << std::setfill('0') << index;
  return id.str();
}

std::filesystem::path item_staging_dir(const BuildBatchOptions& options,
                                       const std::filesystem::path& source) {
  if (options.staging_dir.empty()) {
    return {};
  }
  return options.staging_dir / (source.stem().string() + ".staging");
}

// What to do with an item before any Mac builds it: nullopt to build it,
// otherwise its final result (kept, or failed before building).
std::optional<BuildBatchItemResult> settle_before_build(const BuildBatchOptions& options,
                                                        BuildBatchItemResult item) {
  std::error_code error;
  std::filesystem::create_directories(item.artifact_path.parent_path(), error);
  if (error) {
    item.status = BuildBatchStatus::failed;
    item.error_message = "cannot create " + item.artifact_path.parent_path().string() + ": " +
                         error.message();
    return item;
  }
  if (std::filesystem::equivalent(item.artifact_path, item.source, error)) {
    item.status = BuildBatchStatus::failed;
    item.error_message = "the output would replace the source; choose another --out-dir";
    return item;
  }
  if (!std::filesystem::exists(item.artifact_path, error)) {
    return std::nullopt;
  }
  switch (options.existing) {
    case ExistingOutputs::refuse:
      item.status = BuildBatchStatus::failed;
      item.error_message = "output already exists: " + item.artifact_path.string() +
                           " (pass --resume to keep verified outputs, or --fresh)";
      return item;
    case ExistingOutputs::resume: {
      std::string why;
      if (existing_output_is_complete(options.parameters.output_format, item.artifact_path,
                                      item.source, why)) {
        item.status = BuildBatchStatus::kept;
        return item;
      }
      return std::nullopt;
    }
    case ExistingOutputs::rebuild:
      return std::nullopt;
  }
  return std::nullopt;
}

VideoBuildParameters item_parameters(const BuildBatchOptions& options,
                                     const std::filesystem::path& source) {
  VideoBuildParameters parameters = options.parameters;
  parameters.source_name = source.filename().string();
  return parameters;
}

std::string_view remote_status_label(RemoteVideoStatus status) {
  switch (status) {
    case RemoteVideoStatus::built:
      return "built";
    case RemoteVideoStatus::failed:
      return "failed";
    case RemoteVideoStatus::busy:
      return "busy, asking again later";
    case RemoteVideoStatus::unavailable:
      return "not used for the rest of the batch";
  }
  return "failed";
}

}  // namespace

std::string_view build_batch_status_label(BuildBatchStatus status) noexcept {
  switch (status) {
    case BuildBatchStatus::created:
      return "created";
    case BuildBatchStatus::kept:
      return "kept";
    case BuildBatchStatus::failed:
      return "failed";
    case BuildBatchStatus::cancelled:
      return "cancelled";
  }
  return "failed";
}

bool BuildBatchResult::all_succeeded() const noexcept {
  for (const BuildBatchItemResult& item : items) {
    if (item.status == BuildBatchStatus::failed || item.status == BuildBatchStatus::cancelled) {
      return false;
    }
  }
  return true;
}

std::filesystem::path build_batch_artifact_path(const std::filesystem::path& source,
                                                const std::filesystem::path& out_dir,
                                                VideoOutputFormat format) {
  switch (format) {
    case VideoOutputFormat::svp:
      return out_dir / (source.stem().string() + ".svp");
    case VideoOutputFormat::svpi:
      return out_dir / (source.stem().string() + ".svpi");
    case VideoOutputFormat::embedded_svpi:
      return out_dir / source.filename();
  }
  return out_dir / source.filename();
}

BuildBatchResult build_batch(const BuildBatchOptions& options) {
  BuildBatchResult result;
  result.items.resize(options.sources.size());
  std::map<std::filesystem::path, std::size_t> first_writer;
  std::vector<bool> settled(options.sources.size(), false);
  for (std::size_t index = 0; index < options.sources.size(); ++index) {
    BuildBatchItemResult item;
    // An absolute path for every Mac: a build records its source path as
    // given, and another Mac builds from its own copy, so a relative path
    // would make the package depend on which Mac coordinated it.
    item.source = std::filesystem::absolute(options.sources[index]);
    item.artifact_path = build_batch_artifact_path(item.source, options.out_dir,
                                                   options.parameters.output_format);
    if (options.parameters.run_report) {
      item.run_report_path = item.artifact_path.string() + std::string(kRunReportSuffix);
    }
    const auto [writer, inserted] = first_writer.emplace(item.artifact_path, index);
    if (!inserted) {
      item.status = BuildBatchStatus::failed;
      item.error_message = "another video of this batch (" +
                           options.sources[writer->second].string() +
                           ") writes the same output " + item.artifact_path.string();
      result.items[index] = std::move(item);
      settled[index] = true;
      continue;
    }
    if (std::optional<BuildBatchItemResult> done = settle_before_build(options, item)) {
      result.items[index] = std::move(*done);
      settled[index] = true;
      continue;
    }
    result.items[index] = std::move(item);
  }

  // Only the videos that still need a build are dispatched.
  std::vector<std::size_t> pending;
  for (std::size_t index = 0; index < options.sources.size(); ++index) {
    if (!settled[index]) {
      pending.push_back(index);
    }
  }
  std::mutex mutex;
  // One line per video and Mac as it starts and ends (stderr), so a batch
  // shows which Mac coordinates which video.
  const auto report = [&](const std::filesystem::path& source, const std::string& mac,
                          const std::string& what) {
    if (options.quiet) {
      return;
    }
    const std::lock_guard lock(mutex);
    std::cerr << "svp-builder build-batch: " << source.filename().string() << " on " << mac
              << ": " << what << "\n";
  };
  const auto overwrite = options.existing != ExistingOutputs::refuse;
  dispatch_batch(BatchDispatchOptions{
      .item_count = pending.size(),
      .local_slots = kVideoBuildSlotsPerMac,
      .run_local =
          [&](std::size_t position) {
            const std::size_t index = pending[position];
            BuildBatchItemResult& item = result.items[index];
            VideoBuildRunOptions run{
                .parameters = item_parameters(options, item.source),
                .source_path = item.source,
                .output_path = item.artifact_path,
                .staging_dir = item_staging_dir(options, item.source),
                .model_cache_dir = options.model_cache_dir,
                .ffprobe_path = options.ffprobe_path,
                .ffmpeg_path = options.ffmpeg_path,
                .sherpa_lib_path = options.sherpa_lib_path,
                .runtime_tools = options.runtime_tools,
                .overwrite_output = overwrite,
                .use_parameter_thread_plan = false,
                .distributed = options.make_distributed ? options.make_distributed() : nullptr,
                .progress_sink = options.progress_sink
                                     ? make_scoped_progress_sink(options.progress_sink,
                                                                 item_id(index),
                                                                 item.source.filename().string())
                                     : nullptr,
                .quiet = options.quiet,
                .verbose = options.verbose};
            report(item.source, std::string(kThisMac), "started");
            const VideoBuildRunResult built = options.run_local
                                                  ? options.run_local(run, item.run_report_path)
                                                  : run_video_build(run);
            report(item.source, std::string(kThisMac),
                   built.success ? std::string("built")
                                 : "failed" + (built.error_message.empty()
                                                   ? std::string()
                                                   : ": " + built.error_message));
            const std::lock_guard lock(mutex);
            item.built_on = std::string(kThisMac);
            item.status = built.success     ? BuildBatchStatus::created
                          : built.cancelled ? BuildBatchStatus::cancelled
                                            : BuildBatchStatus::failed;
            item.error_message = built.error_message;
          },
      .remote_macs = options.coordinators,
      .run_remote =
          [&](std::size_t position, RemoteVideoBuilder& mac) {
            const std::size_t index = pending[position];
            const BuildBatchItemResult& item = result.items[index];
            report(item.source, mac.name(), "started");
            const RemoteVideoOutcome outcome = mac.build(RemoteVideoRequest{
                .item_id = item_id(index),
                .parameters = item_parameters(options, item.source),
                .source_path = item.source,
                .output_path = item.artifact_path,
                .run_report_path = item.run_report_path});
            report(item.source, mac.name(),
                   std::string(remote_status_label(outcome.status)) +
                       (outcome.message.empty() ? std::string() : ": " + outcome.message));
            const std::lock_guard lock(mutex);
            BuildBatchItemResult& updated = result.items[index];
            switch (outcome.status) {
              case RemoteVideoStatus::built:
                updated.status = BuildBatchStatus::created;
                updated.built_on = mac.name();
                updated.error_message.clear();
                break;
              case RemoteVideoStatus::failed:
                updated.status = BuildBatchStatus::failed;
                updated.built_on = mac.name();
                updated.error_message = outcome.message;
                break;
              case RemoteVideoStatus::busy:
                break;
              case RemoteVideoStatus::unavailable:
                result.dropped_coordinators.push_back(mac.name() + ": " + outcome.message);
                break;
            }
            return outcome;
          },
      .busy_backoff = kRemoteVideoBusyBackoff});
  return result;
}

}  // namespace svp::builder::batch
