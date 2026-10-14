#include "ocr_generation_internal.hpp"

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/evidence_crop.hpp"
#include "svp/vision/evidence_crop_work.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sys/wait.h>

namespace svp::vision::ocr_generation_internal {
namespace {

std::vector<CropGenerationInput> build_crop_inputs(
    const OcrGenerationResult& result,
    const std::vector<ReconciledObservation>& reconciled) {
  std::vector<CropGenerationInput> crop_inputs;
  crop_inputs.reserve(result.text_observations.size());

  for (std::size_t i = 0; i < result.text_observations.size(); ++i) {
    const auto& obs = result.text_observations[i];
    const auto& robs = reconciled[i];

    CropGenerationInput input;
    input.text_region_id = obs.text_region_id;
    input.text_observation_id = obs.text_observation_id;
    if (!obs.source_frame_ids.empty()) {
      input.source_frame_id = obs.source_frame_ids[0];
    }
    input.source_timestamp_us = robs.start_us;
    input.bbox_left = robs.bbox_left;
    input.bbox_top = robs.bbox_top;
    input.bbox_right = robs.bbox_right;
    input.bbox_bottom = robs.bbox_bottom;
    input.frame_width = robs.frame_width;
    input.frame_height = robs.frame_height;
    input.confidence = robs.confidence;
    input.detection_count = robs.detection_count;
    input.observation_raw_text = obs.raw_text;
    crop_inputs.push_back(std::move(input));
  }

  return crop_inputs;
}

EvidenceCropOptions build_crop_options(const OcrGenerationOptions& options) {
  const OcrSourceFrameDimensions source_dims =
      derive_ocr_source_frame_dimensions(
          static_cast<int>(options.media_plan->primary_video_stream.width),
          static_cast<int>(options.media_plan->primary_video_stream.height),
          static_cast<int>(options.media_plan->primary_video_stream.rotation_degrees));

  EvidenceCropOptions crop_opts;
  crop_opts.ffmpeg_path = options.ffmpeg_path;
  crop_opts.source_media_path = options.media_plan->source_path;
  crop_opts.ocr_frame_width = options.ocr_frame_width;
  crop_opts.ocr_frame_height = options.ocr_frame_height;
  crop_opts.source_frame_width = source_dims.width;
  crop_opts.source_frame_height = source_dims.height;
  crop_opts.canonical_raster_width = options.canonical_raster_width;
  crop_opts.canonical_raster_height = options.canonical_raster_height;
  crop_opts.max_total_crops = options.max_total_crops;
  crop_opts.max_total_crop_bytes = options.max_total_crop_bytes;
  crop_opts.crop_coverage_policy = options.crop_coverage_policy;
  crop_opts.min_jpeg_quality = options.crop_min_jpeg_quality;
  crop_opts.crop_image_format = kEvidenceCropImageFormat;
  crop_opts.jpeg_quality = kEvidenceCropJpegQuality;
  crop_opts.on_progress = options.on_evidence_crop_progress;
  return crop_opts;
}

void copy_crop_result_to_generation_result(
    const EvidenceCropResult& crop_result,
    OcrGenerationResult& result) {
  result.evidence_crops = crop_result.crops;
  result.evidence_crop_count = crop_result.crop_count;
  result.evidence_crop_total_bytes = crop_result.total_crop_bytes;
  result.evidence_crops_written = crop_result.crops_written;
  result.evidence_crops_skipped = crop_result.crops_skipped_count;
  result.evidence_crops_skipped_reason = crop_result.crops_skipped_reason;
  result.crop_coverage_policy = crop_result.crop_coverage_policy;
  result.crop_effective_max_total_crops = crop_result.effective_max_total_crops;
  result.crop_effective_max_total_crop_bytes = crop_result.effective_max_total_crop_bytes;
  result.crops_skipped_by_count_cap = crop_result.crops_skipped_by_count_cap;
  result.crops_skipped_by_byte_cap = crop_result.crops_skipped_by_byte_cap;
  result.crops_skipped_by_extraction = crop_result.crops_skipped_by_extraction;
  result.crop_total_observations_requested = crop_result.total_observations_requested;
  result.every_observation_has_crop = crop_result.every_observation_has_crop;
  result.crop_coverage_status = crop_result.crop_coverage_status;
}

void rewrite_evidence_crop_jsonl(const std::filesystem::path& staging_dir,
                                 const OcrGenerationResult& result) {
  std::ofstream out(staging_dir / "text" / "evidence_crops.jsonl");
  if (!out) return;
  for (const auto& crop : result.evidence_crops) {
    out << evidence_crop_to_json(crop).dump() << "\n";
  }
}

// Crop work a dispatcher already did (dispatched_work.hpp): its successful
// outcomes by crop-input index, each with the job it was computed for. The
// stage writes these bytes and takes these ROI re-reads instead of running
// ffmpeg and PP-OCR again; every other crop, a failed one included, is
// computed here exactly as without a dispatcher.
struct DispatchedCrops {
  std::vector<std::optional<EvidenceCropJob>> jobs;
  std::vector<std::optional<EvidenceCropJobOutcome>> outcomes;
  // Set by the image writer: input i's crop file holds the dispatched bytes.
  std::vector<bool> written;
};

std::optional<DispatchedCrops> dispatch_evidence_crops(
    const OcrGenerationOptions& options,
    const EvidenceCropOptions& crop_options,
    const std::vector<CropGenerationInput>& crop_inputs,
    const PpOcrOptions& roi_options) {
  if (!options.evidence_crop_dispatcher || !evidence_crop_caps_never_bind(crop_options)) {
    return std::nullopt;
  }
  const std::vector<EvidenceCropJob> jobs = plan_evidence_crop_jobs(crop_options, crop_inputs);
  const std::size_t input_count = crop_inputs.size();
  const auto report = [&options, input_count](std::size_t done, std::size_t total) {
    if (options.on_evidence_crop_progress && total > 0) {
      // Extraction progress is per crop input, as the stage reports it.
      options.on_evidence_crop_progress(done * input_count / total, input_count);
    }
  };
  std::optional<std::vector<EvidenceCropJobOutcome>> outcomes =
      options.evidence_crop_dispatcher(jobs, roi_options, report);
  if (!outcomes) {
    return std::nullopt;
  }
  if (outcomes->size() != jobs.size()) {
    throw DispatchedWorkError("evidence crops: " + std::to_string(outcomes->size()) +
                              " outcomes for " + std::to_string(jobs.size()) + " crops");
  }
  DispatchedCrops dispatched;
  dispatched.jobs.resize(input_count);
  dispatched.outcomes.resize(input_count);
  dispatched.written.assign(input_count, false);
  for (std::size_t index = 0; index < jobs.size(); ++index) {
    EvidenceCropJobOutcome& outcome = (*outcomes)[index];
    if (outcome.ordinal != jobs[index].ordinal) {
      throw DispatchedWorkError("evidence crops: outcome " + std::to_string(index) +
                                " is for crop " + std::to_string(outcome.ordinal) +
                                ", not " + std::to_string(jobs[index].ordinal));
    }
    if (!outcome.extracted) {
      continue;  // computed again here
    }
    dispatched.jobs[jobs[index].ordinal] = jobs[index];
    dispatched.outcomes[jobs[index].ordinal] = std::move(outcome);
  }
  if (options.on_evidence_crop_progress) {
    options.on_evidence_crop_progress(input_count, input_count);
  }
  return dispatched;
}

bool write_image_bytes(const std::vector<std::byte>& bytes, const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  return static_cast<bool>(out);
}

EvidenceCropImageWriter dispatched_image_writer(const EvidenceCropOptions& crop_options,
                                                DispatchedCrops& dispatched) {
  return [&crop_options, &dispatched](const EvidenceCropJob& job,
                                      const std::filesystem::path& path, std::string& error) {
    const std::size_t index = static_cast<std::size_t>(job.ordinal);
    if (index < dispatched.outcomes.size()) {
      dispatched.written[index] = false;
      if (dispatched.outcomes[index] && dispatched.jobs[index] == job &&
          write_image_bytes(dispatched.outcomes[index]->image, path)) {
        dispatched.written[index] = true;
        return true;
      }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return extract_evidence_crop_image(crop_options.ffmpeg_path,
                                       crop_options.source_media_path, job, path, error);
  };
}

// The dispatched ROI re-read of input `index`, when its crop file holds the
// dispatched bytes and the re-read found text. A re-read that could not
// decode the crop or produced no text is done again here: PP-OCR reports a
// failed inference as empty text, so only text is taken as settled.
std::optional<EvidenceCropRoi> dispatched_roi(const DispatchedCrops* dispatched,
                                              std::optional<std::size_t> index) {
  if (dispatched == nullptr || !index || *index >= dispatched->written.size() ||
      !dispatched->written[*index]) {
    return std::nullopt;
  }
  const EvidenceCropRoi& roi = dispatched->outcomes[*index]->roi;
  if (!roi.decoded || roi.text.empty()) {
    return std::nullopt;
  }
  return roi;
}

}  // namespace

RoiHardeningSummary generate_and_harden_evidence_crops(
    const OcrGenerationOptions& options,
    const std::vector<ReconciledObservation>& reconciled,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir,
    OcrGenerationResult& result) {
  RoiHardeningSummary summary;
  if (!options.generate_evidence_crops ||
      options.media_plan == nullptr ||
      result.text_observations.empty()) {
    return summary;
  }

  EvidenceCropResult crop_result;
  std::optional<EvidenceCropOptions> crop_options;
  std::vector<CropGenerationInput> crop_inputs;
  std::optional<DispatchedCrops> dispatched;
  // As one step, as a build without a dispatcher has always done it: if
  // either throws, the stage records that error and makes no crops.
  bool prepared = false;
  try {
    crop_options = build_crop_options(options);
    crop_inputs = build_crop_inputs(result, reconciled);
    prepared = true;
  } catch (const std::exception& e) {
    crop_result.crops_written = false;
    crop_result.crops_skipped_reason =
        std::string("Evidence crop generation error: ") + e.what();
  }
  if (prepared) {
    // Outside the try: a dispatcher that fails ends the build
    // (DispatchedWorkError), never as a crop blocker.
    dispatched = dispatch_evidence_crops(options, *crop_options, crop_inputs, pp_ocr_opts);
    EvidenceCropOptions writing_options = *crop_options;
    if (dispatched) {
      // Extraction progress was reported as the dispatched work arrived.
      writing_options.on_progress = nullptr;
    }
    try {
      crop_result = generate_evidence_crops_internal(
          writing_options,
          crop_inputs,
          staging_dir,
          dispatched ? dispatched_image_writer(*crop_options, *dispatched)
                     : EvidenceCropImageWriter{});
    } catch (const std::exception& e) {
      crop_result.crops_written = false;
      crop_result.crops_skipped_reason =
          std::string("Evidence crop generation error: ") + e.what();
    }
  }

  copy_crop_result_to_generation_result(crop_result, result);
  result.roi_hardening_run = true;

  std::map<std::string, std::size_t> observation_index_by_id;
  for (std::size_t i = 0; i < result.text_observations.size(); ++i) {
    observation_index_by_id[result.text_observations[i].text_observation_id] = i;
  }
  std::map<std::string, std::size_t> crop_input_index_by_observation;
  for (std::size_t i = 0; i < crop_inputs.size(); ++i) {
    crop_input_index_by_observation.emplace(crop_inputs[i].text_observation_id, i);
  }

  if (options.on_evidence_roi_progress) {
    options.on_evidence_roi_progress(0, result.evidence_crops.size());
  }
  std::size_t processed_crop_count = 0;
  for (auto& crop : result.evidence_crops) {
    auto report_crop_processed = [&]() {
      ++processed_crop_count;
      if (options.on_evidence_roi_progress) {
        options.on_evidence_roi_progress(processed_crop_count,
                                         result.evidence_crops.size());
      }
    };

    auto obs_it = observation_index_by_id.find(crop.text_observation_id);
    if (obs_it == observation_index_by_id.end()) {
      crop.evidence_quality = "weak";
      crop.evidence_quality_reason = "Linked text observation was not found";
      report_crop_processed();
      continue;
    }

    auto& obs = result.text_observations[obs_it->second];
    if (std::find(obs.evidence_crop_refs.begin(),
                  obs.evidence_crop_refs.end(),
                  crop.crop_id) == obs.evidence_crop_refs.end()) {
      obs.evidence_crop_refs.push_back(crop.crop_id);
    }

    std::optional<std::size_t> crop_input_index;
    if (const auto found = crop_input_index_by_observation.find(crop.text_observation_id);
        found != crop_input_index_by_observation.end()) {
      crop_input_index = found->second;
    }
    std::optional<EvidenceCropRoi> roi =
        dispatched_roi(dispatched ? &*dispatched : nullptr, crop_input_index);
    if (!roi) {
      const int crop_width = crop.crop_bbox_right - crop.crop_bbox_left;
      const int crop_height = crop.crop_bbox_bottom - crop.crop_bbox_top;
      roi = reread_evidence_crop(
          pp_ocr_session, pp_ocr_opts,
          decode_evidence_crop_image(
              options.ffmpeg_path,
              staging_dir / crop.crop_file_path,
              crop_width,
              crop_height,
              crop.source_frame_id,
              crop.source_timestamp_us));
    }
    if (!roi->decoded) {
      crop.evidence_quality = "unsupported";
      crop.evidence_quality_reason =
          "Crop image could not be decoded for ROI OCR verification";
      report_crop_processed();
      continue;
    }

    if (roi->text.empty()) {
      crop.evidence_quality = "weak";
      crop.evidence_quality_reason = "Crop was decoded but ROI OCR produced no text";
      report_crop_processed();
      continue;
    }

    crop.roi_ocr_text = roi->text;
    crop.roi_ocr_confidence = roi->score;
    crop.roi_ocr_word_count =
        alphanumeric_key(normalize_text(roi->text)).empty() ? 0 : 1;

    if (roi_text_is_better(obs.raw_text, roi->text)) {
      obs.raw_text = roi->text;
      obs.normalized_text = normalize_text(roi->text);
      obs.confidence = std::max(obs.confidence, roi->score);
      if (obs.language.has_value()) {
        (*obs.language)["confidence"] = obs.confidence;
      }
      ++summary.improved_observation_count;
    }

    crop.evidence_quality = "strong";
    crop.evidence_quality_reason =
        "Crop was decoded and PP-OCR ROI re-read produced text for the linked observation";
    ++summary.verified_crop_count;
    report_crop_processed();
  }

  rewrite_evidence_crop_jsonl(staging_dir, result);
  return summary;
}

}  // namespace svp::vision::ocr_generation_internal
