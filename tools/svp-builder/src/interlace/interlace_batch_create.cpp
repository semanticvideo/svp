#include "svp/builder/interlace.hpp"
#include "svp/builder/build_pipeline.hpp"
#include "svp/builder/interlace_batch.hpp"
#include "svp/builder/build_progress.hpp"
#include "interlace_batch_internal.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace svp::builder {

namespace {

nlohmann::json make_svpi_manifest(
    const std::string& source_filename,
    const svp::package::MediaBindingDocument& binding_doc) {
  nlohmann::json sections = nlohmann::json::object();
  sections["transcript"] = {{"state", "not_generated"}};
  sections["timeline"] = {{"state", "not_generated"}};
  sections["text"] = {{"state", "not_generated"}};
  sections["colors"] = {{"state", "not_generated"}};
  sections["entities"] = {{"state", "not_generated"}};
  sections["spatial"] = {{"state", "not_generated"}};
  sections["relationships"] = {{"state", "not_generated"}};
  sections["embeddings"] = {{"state", "not_generated"}};

  std::string package_id =
      "svpi_" + std::filesystem::path(source_filename).stem().string() + "_pkg";

  return {
    {"format", "svpi"},
    {"svpi_version", std::string{svp::package::kSvpiVersion}},
    {"svp_version", "1.0-rc.2"},
    {"package_id", package_id},
    {"created_utc", make_utc_timestamp()},
    {"media_binding_ref", "media_binding.json"},
    {"primary_media_binding_id", binding_doc.primary_binding_id},
    {"timebase", {
      {"unit", "microseconds"},
      {"origin", "primary_presentation_start"},
      {"source_timebase_mode", "exact_rational"},
      {"rounding", "round_half_to_even"}
    }},
    {"sections", sections}
  };
}

bool create_single_svpi(
    const std::filesystem::path& source_path,
    const std::filesystem::path& svpi_output_path,
    const std::string& ffprobe_path,
    const std::string& ffmpeg_path,
    bool compute_full_blake3,
    const std::string& staging_dir_override,
    const std::string& model_cache_dir,
    const std::string& sherpa_lib_path,
    const svp::vision::InferencePerformanceOptions& performance,
    bool core_only_diagnostic,
    bool allow_fallback_diarization,
    bool force_single_speaker,
    bool serial_pipeline,
    std::string& error_message,
    std::string& blake3_state_out,
    const std::shared_ptr<BuildProgressSink>& progress_sink) {

  svp::builder::InterlaceCreateOptions opts;
  opts.source_path = source_path.string();
  opts.output_path = svpi_output_path.string();
  opts.ffprobe_path = ffprobe_path;
  opts.ffmpeg_path = ffmpeg_path;
  opts.compute_full_blake3 = compute_full_blake3;
  opts.staging_dir = staging_dir_override;
  opts.model_cache_dir = model_cache_dir;
  opts.sherpa_lib_path = sherpa_lib_path;
  opts.performance = performance;
  opts.core_only_diagnostic = core_only_diagnostic;
  opts.allow_fallback_diarization = allow_fallback_diarization;
  opts.force_single_speaker = force_single_speaker;
  opts.serial_pipeline = serial_pipeline;
  opts.progress_sink = progress_sink;

  auto result = svp::builder::interlace_create(opts);
  blake3_state_out = result.blake3_state;
  if (!result.success) {
    error_message = result.error_message;
    return false;
  }
  return true;
}

std::string batch_item_staging_dir(
    const std::string& staging_dir_override,
    const std::string& source_relative_path) {
  if (staging_dir_override.empty()) {
    return {};
  }

  std::filesystem::path item_staging =
      std::filesystem::path(staging_dir_override) /
      std::filesystem::path(source_relative_path);
  item_staging.replace_extension(".staging");
  return item_staging.string();
}

bool check_svpi_valid_and_bound(
    const std::filesystem::path& svpi_path,
    const std::filesystem::path& media_path,
    const std::string& validation_codes_path,
    std::string& error_message) {

  svp::validation::SvpiValidatorOptions vopts;
  vopts.validation_codes_path = validation_codes_path;
  auto report = svp::validation::validate_svpi_package(svpi_path, vopts);
  if (svp::validation::exit_code(report) != 0) {
    error_message = "SVPI structure validation failed";
    for (const auto& err : report.errors) {
      error_message += "\n  " + err.code + ": " + err.message;
    }
    return false;
  }

  auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
  if (!binding_entry.has_value()) {
    error_message = "could not read media_binding.json";
    return false;
  }

  auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());
  auto verification = svp::package::verify_media_binding(media_path, binding_doc);

  if (verification.state != svp::package::BindingVerificationState::verified) {
    error_message = "binding mismatch: " + verification.state_label;
    return false;
  }

  return true;
}

}  // namespace

BatchCreateResult interlace_create_batch(const BatchCreateOptions& options) {
  BatchCreateResult result;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    BatchFileResult r;
    r.status = BatchFileStatus::failed;
    r.error_message = "source directory does not exist: " + options.source_dir;
    result.results.push_back(std::move(r));
    result.failed_count = 1;
    return result;
  }

  const std::filesystem::path out_dir =
      options.out_dir.empty() || options.out_dir == "same-as-source"
          ? source_dir
          : std::filesystem::path(options.out_dir);

  if (options.output_format == BatchOutputFormat::embedded_svpi) {
    const bool has_output_directory =
        !options.out_dir.empty() && options.out_dir != "same-as-source";
    if (!has_output_directory && !options.overwrite_sources) {
      BatchFileResult r;
      r.status = BatchFileStatus::failed;
      r.error_message =
          "Embedded SVPI batch output requires an explicit output directory or source overwrite permission.";
      result.results.push_back(std::move(r));
      result.failed_count = 1;
      return result;
    }
    if (has_output_directory && options.overwrite_sources) {
      BatchFileResult r;
      r.status = BatchFileStatus::failed;
      r.error_message =
          "Source overwrite permission cannot be combined with an output directory.";
      result.results.push_back(std::move(r));
      result.failed_count = 1;
      return result;
    }

    if (has_output_directory) {
      std::error_code canonical_error;
      const auto canonical_source = std::filesystem::weakly_canonical(
          source_dir, canonical_error);
      canonical_error.clear();
      const auto canonical_output = std::filesystem::weakly_canonical(
          out_dir, canonical_error);
      if (!canonical_error) {
        const auto relative_output = canonical_output.lexically_relative(
            canonical_source);
        const bool output_is_source = relative_output.empty() ||
                                      relative_output == ".";
        const bool output_is_descendant = !relative_output.empty() &&
            *relative_output.begin() != "..";
        if (output_is_source || output_is_descendant) {
          BatchFileResult r;
          r.status = BatchFileStatus::failed;
          r.error_message =
              "Embedded SVPI output directory must be separate from and outside the source directory.";
          result.results.push_back(std::move(r));
          result.failed_count = 1;
          return result;
        }
      }
    }
  }

  if (!std::filesystem::exists(out_dir)) {
    std::filesystem::create_directories(out_dir);
  }

  if (options.output_format == BatchOutputFormat::svpi &&
      options.visibility == SidecarVisibility::managed_dir) {
    std::filesystem::create_directories(out_dir / ".svpi");
  }

  sink->emit(make_stage_started(ProgressStageId::batch_scan, options.source_dir));
  auto media_files = discover_media_files(source_dir, options.recursive);
  sink->emit(make_stage_completed(ProgressStageId::batch_scan,
      std::to_string(media_files.size()) + " media files found"));

  result.results.resize(media_files.size());

  const BuilderConcurrencyPolicy policy = builder_concurrency_policy(
      options.performance,
      static_cast<std::size_t>(std::max(1, options.jobs)));
  const std::size_t worker_count =
      std::min<std::size_t>(media_files.size(), policy.max_batch_jobs);
  std::atomic<std::size_t> next_index{0};

  auto process_item = [&](std::size_t index) {
    const auto& media_path = media_files[index];
    BatchFileResult file_result;
    file_result.source_filename = media_path.filename().string();
    const auto source_relative_path =
        media_path.lexically_normal().lexically_relative(
            source_dir.lexically_normal());
    file_result.source_relative_path = source_relative_path.string();

    auto item_sink = make_scoped_progress_sink(
        sink,
        file_result.source_relative_path,
        file_result.source_relative_path.empty()
            ? file_result.source_filename
            : file_result.source_relative_path);

    item_sink->emit(make_stage_started(ProgressStageId::batch_item,
        media_path.filename().string()));

    const bool use_out_dir =
        !options.out_dir.empty() && options.out_dir != "same-as-source";
    const auto local_out_dir =
        use_out_dir ? out_dir : media_path.parent_path();

    file_result.artifact_path =
        options.output_format == BatchOutputFormat::embedded_svpi &&
                options.overwrite_sources
            ? media_path
            : resolve_batch_artifact_path(
                  media_path, source_dir, local_out_dir,
                  options.output_format, options.visibility);

    const bool requires_contained_output =
        options.output_format == BatchOutputFormat::embedded_svpi &&
        !options.overwrite_sources;
    std::string containment_error;
    if (requires_contained_output && !batch_artifact_is_contained(
            file_result.artifact_path, out_dir, containment_error)) {
      file_result.status = BatchFileStatus::failed;
      file_result.error_message = containment_error;
      item_sink->emit(make_stage_completed(
          ProgressStageId::batch_item, media_path.filename().string()));
      result.results[index] = std::move(file_result);
      return;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        file_result.artifact_path.parent_path(), directory_error);
    if (directory_error ||
        (requires_contained_output && !batch_artifact_is_contained(
            file_result.artifact_path, out_dir, containment_error))) {
      file_result.status = BatchFileStatus::failed;
      file_result.error_message = directory_error
          ? "Could not create the batch artifact directory: " +
                directory_error.message()
          : containment_error;
      item_sink->emit(make_stage_completed(
          ProgressStageId::batch_item, media_path.filename().string()));
      result.results[index] = std::move(file_result);
      return;
    }

    const auto staging_dir = batch_item_staging_dir(
        options.staging_dir, file_result.source_relative_path);

    if (options.output_format == BatchOutputFormat::embedded_svpi &&
        options.overwrite_sources) {
      std::string inspection_error;
      const auto state = inspect_embedded_batch_artifact(
          media_path, media_path, {},
          inspection_error);
      if (state == EmbeddedBatchArtifactState::valid) {
        file_result.status = BatchFileStatus::already_valid;
      } else if (state == EmbeddedBatchArtifactState::invalid) {
        file_result.status = BatchFileStatus::failed;
        file_result.error_message = inspection_error;
      } else {
        std::string blake3_state;
        std::string create_error;
        if (create_embedded_batch_artifact(
                options, media_path, media_path, staging_dir, create_error,
                blake3_state, item_sink, true)) {
          file_result.status = BatchFileStatus::created;
          file_result.blake3_state = blake3_state;
        } else {
          file_result.status = BatchFileStatus::failed;
          file_result.error_message = create_error;
        }
      }
    } else if (std::filesystem::exists(file_result.artifact_path)) {
      std::string err;
      const bool existing_valid =
          options.output_format == BatchOutputFormat::svpi
              ? check_svpi_valid_and_bound(
                    file_result.artifact_path, media_path, {}, err)
              : check_embedded_batch_artifact(
                    file_result.artifact_path, media_path, {}, err);
      if (existing_valid) {
        file_result.status = BatchFileStatus::already_valid;
      } else {
        if (options.replace_mismatched) {
          std::string blake3_state;
          std::string create_err;
          const bool replaced =
              options.output_format == BatchOutputFormat::svpi
                  ? create_single_svpi(
                        media_path, file_result.artifact_path,
                        options.ffprobe_path, options.ffmpeg_path,
                        !options.no_blake3, staging_dir,
                        options.model_cache_dir, options.sherpa_lib_path,
                        options.performance, options.core_only_diagnostic,
                        options.allow_fallback_diarization,
                        options.force_single_speaker,
                        options.serial_pipeline, create_err, blake3_state,
                        item_sink)
                  : create_embedded_batch_artifact(
                        options, media_path, file_result.artifact_path,
                        staging_dir, create_err, blake3_state, item_sink,
                        true);
          if (replaced) {
            file_result.status = BatchFileStatus::replaced;
            file_result.blake3_state = blake3_state;
          } else {
            file_result.status = BatchFileStatus::failed;
            file_result.error_message = create_err;
          }
        } else {
          file_result.status = BatchFileStatus::binding_mismatch;
          file_result.error_message = err;
        }
      }
    } else {
      std::string blake3_state;
      std::string create_err;
      const bool created =
          options.output_format == BatchOutputFormat::svpi
              ? create_single_svpi(
                    media_path, file_result.artifact_path,
                    options.ffprobe_path, options.ffmpeg_path,
                    !options.no_blake3, staging_dir,
                    options.model_cache_dir, options.sherpa_lib_path,
                    options.performance, options.core_only_diagnostic,
                    options.allow_fallback_diarization,
                    options.force_single_speaker,
                    options.serial_pipeline, create_err, blake3_state,
                    item_sink)
              : create_embedded_batch_artifact(
                    options, media_path, file_result.artifact_path,
                    staging_dir, create_err, blake3_state, item_sink,
                    false);
      if (created) {
        file_result.status = BatchFileStatus::created;
        file_result.blake3_state = blake3_state;
      } else {
        file_result.status = BatchFileStatus::failed;
        file_result.error_message = create_err;
      }
    }

    item_sink->emit(make_stage_completed(ProgressStageId::batch_item,
        media_path.filename().string()));
    result.results[index] = std::move(file_result);
  };

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&]() {
      while (true) {
        const std::size_t index = next_index.fetch_add(1);
        if (index >= media_files.size()) {
          return;
        }
        process_item(index);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  for (const auto& file_result : result.results) {
    switch (file_result.status) {
      case BatchFileStatus::created:
        ++result.created_count;
        break;
      case BatchFileStatus::already_valid:
        ++result.already_valid_count;
        break;
      case BatchFileStatus::skipped_unsupported:
        ++result.skipped_count;
        break;
      case BatchFileStatus::binding_mismatch:
        ++result.mismatch_count;
        break;
      case BatchFileStatus::failed:
        ++result.failed_count;
        break;
      case BatchFileStatus::replaced:
        ++result.replaced_count;
        break;
    }
  }

  return result;
}

}  // namespace svp::builder
