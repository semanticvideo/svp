#include "svp/builder/interlace_batch.hpp"
#include "svp/builder/build_progress.hpp"
#include "interlace_batch_internal.hpp"

#include "svp/package/media_binding.hpp"
#include "svp/package/media_binding_factory.hpp"
#include "svp/package/package_layout.hpp"
#include "svp/validation/svpi_validator.hpp"
#include "svp/validation/report_json.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace svp::builder {

BatchValidateResult interlace_validate_batch(const BatchValidateOptions& options) {
  BatchValidateResult result;

  std::shared_ptr<BuildProgressSink> sink = options.progress_sink;
  if (!sink) {
    sink = default_progress_sink();
  }

  const std::filesystem::path source_dir(options.source_dir);
  if (!std::filesystem::exists(source_dir) || !std::filesystem::is_directory(source_dir)) {
    return result;
  }

  sink->emit(make_stage_started(ProgressStageId::batch_scan, options.source_dir));
  auto svpi_files = discover_svpi_files(source_dir, options.recursive);
  sink->emit(make_stage_completed(ProgressStageId::batch_scan,
      std::to_string(svpi_files.size()) + " SVPI files found"));

  for (const auto& svpi_path : svpi_files) {
    sink->emit(make_stage_started(ProgressStageId::batch_item,
        svpi_path.filename().string()));

    BatchValidateFileResult file_result;
    file_result.svpi_path = svpi_path;
    file_result.svpi_filename = svpi_path.filename().string();
    file_result.svpi_relative_path =
        std::filesystem::relative(svpi_path, source_dir).string();

    svp::validation::SvpiValidatorOptions vopts;
    vopts.validation_codes_path = options.validation_codes_path;
    auto report = svp::validation::validate_svpi_package(svpi_path, vopts);

    if (svp::validation::exit_code(report) != 0) {
      file_result.state = BatchValidationState::invalid_structure;
      for (const auto& err : report.errors) {
        file_result.errors.push_back(err.code + ": " + err.message);
      }
      result.invalid_structure_count++;
      result.results.push_back(std::move(file_result));
      sink->emit(make_stage_failed(ProgressStageId::batch_item,
          svpi_path.filename().string() + ": invalid structure"));
      continue;
    }

    auto binding_entry = svp::package::read_package_entry(svpi_path, "media_binding.json");
    if (!binding_entry.has_value()) {
      file_result.state = BatchValidationState::invalid_structure;
      file_result.errors.push_back("could not read media_binding.json");
      result.invalid_structure_count++;
      result.results.push_back(std::move(file_result));
      sink->emit(make_stage_failed(ProgressStageId::batch_item,
          svpi_path.filename().string() + ": could not read media_binding.json"));
      continue;
    }

    auto binding_doc = svp::package::parse_media_binding_json(binding_entry.value());

    std::filesystem::path media_dir = media_search_dir_for_svpi(svpi_path);
    std::string stem = sidecar_stem(svpi_path);

    std::vector<std::filesystem::path> media_candidates;
    for (auto ext : kSupportedVideoExts) {
      media_candidates.push_back(media_dir / (stem + std::string(ext)));
    }

    std::filesystem::path found_media;
    for (const auto& candidate : media_candidates) {
      if (std::filesystem::exists(candidate)) {
        found_media = candidate;
        break;
      }
    }

    if (found_media.empty()) {
      file_result.state = BatchValidationState::valid_unbound;
      result.valid_unbound_count++;
    } else {
      file_result.media_filename = found_media.filename().string();
      auto verification = svp::package::verify_media_binding(found_media, binding_doc);
      if (verification.state == svp::package::BindingVerificationState::verified) {
        file_result.state = BatchValidationState::valid_bound;
        result.valid_bound_count++;
      } else {
        file_result.state = BatchValidationState::binding_mismatch;
        for (const auto& check : verification.failing_checks) {
          file_result.errors.push_back(check);
        }
        result.mismatch_count++;
      }
    }

    result.results.push_back(std::move(file_result));
    sink->emit(make_stage_completed(ProgressStageId::batch_item,
        svpi_path.filename().string()));
  }

  return result;
}

}  // namespace svp::builder
