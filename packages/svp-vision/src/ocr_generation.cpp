#include "svp/vision/ocr_generation.hpp"

#include "ocr_generation/ocr_generation_internal.hpp"

#include "svp/core/memory_diagnostics.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/foundation_ocr_staging.hpp"
#include "svp/vision/ocr_frame_batch.hpp"
#include "svp/vision/ocr_frame_batch_reduction.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"
#include "svp/vision/pp_ocr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace svp::vision {
namespace {

using ocr_generation_internal::CollectedOcrFrames;
using ocr_generation_internal::OcrSampleFrame;
using ocr_generation_internal::collect_ocr_sample;
using ocr_generation_internal::complete_ocr_generation;
using ocr_generation_internal::finish_ocr_not_executed;
using ocr_generation_internal::ocr_frame_input_blocker;
using ocr_generation_internal::register_ocr_sample_frames;
using ocr_generation_internal::summarize_ocr_sample_decoding;

// The reducer after batch assembly: frame registration in sample order, the
// per-frame fold, then the unchanged reconciliation and staging.
OcrGenerationResult reduce_ordered_samples(
    OcrGenerationResult result,
    const OcrGenerationOptions& options,
    const std::vector<OcrSampleDetections>& samples,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir) {
  const std::vector<std::optional<OcrSampleFrame>> frames =
      register_ocr_sample_frames(samples, options.frame_catalog);
  CollectedOcrFrames collected;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    if (frames[index].has_value()) {
      collect_ocr_sample(samples[index], *frames[index], collected);
    }
  }
  const DecodedCanonicalFrames decode_status =
      summarize_ocr_sample_decoding(samples);
  result.ocr_frame_input_available =
      decode_status.decoding_succeeded && decode_status.frames_decoded > 0;
  return complete_ocr_generation(std::move(result), options, collected,
                                 decode_status, pp_ocr_session, pp_ocr_opts,
                                 staging_dir);
}

// Local execution of the batched stage: every frame batch runs in this
// process, in plan order, on the one PP-OCR session the evidence crops use.
OcrGenerationResult run_sample_plan_in_process(
    OcrGenerationResult result,
    const OcrGenerationOptions& options,
    const OcrSamplePlan& plan,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir) {
  svp::core::check_memory_limit("ocr.generation.streaming_begin", {
      {"sample_count", std::to_string(plan.samples.size())},
      {"diagnostic_timestamp_override",
       std::getenv(kOcrDiagnosticTimestampsEnv) != nullptr ? "true" : "false"},
      {"ocr_frame_width", std::to_string(options.ocr_frame_width)},
      {"ocr_frame_height", std::to_string(options.ocr_frame_height)}
  });

  int decoded_count = 0;
  const int total = static_cast<int>(plan.samples.size());
  OcrFrameBatchHooks hooks;
  hooks.on_sample_decoded = [&](const OcrSample&) {
    ++decoded_count;
    if (options.on_progress) options.on_progress(decoded_count, total);
  };

  std::vector<std::vector<OcrSampleDetections>> batch_results;
  for (const OcrSampleBatch& batch :
       partition_ocr_samples(plan.samples.size(), options.batch_policy)) {
    const auto first = plan.samples.begin() +
        static_cast<std::ptrdiff_t>(batch.first_ordinal);
    OcrFrameBatchRequest request{
        .source_path = options.media_plan->source_path,
        .ffmpeg_path = options.ffmpeg_path,
        .frame_width = plan.frame_width,
        .frame_height = plan.frame_height,
        .samples = std::vector<OcrSample>(
            first, first + static_cast<std::ptrdiff_t>(batch.count)),
    };
    OcrFrameBatchOutcome outcome =
        run_ocr_frame_batch(pp_ocr_session, pp_ocr_opts, request, hooks);
    if (!outcome.decoding_attempted) {
      DecodedCanonicalFrames decode_status;
      decode_status.skipped_reason = std::move(outcome.skipped_reason);
      result.ocr_frame_input_available = false;
      return complete_ocr_generation(std::move(result), options, {},
                                     decode_status, pp_ocr_session,
                                     pp_ocr_opts, staging_dir);
    }
    batch_results.push_back(std::move(outcome.samples));
  }

  return reduce_ordered_samples(
      std::move(result), options,
      assemble_ocr_frame_batches(plan, std::move(batch_results)),
      pp_ocr_session, pp_ocr_opts, staging_dir);
}

// OCR over the canonical frames decoded by the caller, for options without a
// media plan or decode size.
OcrGenerationResult run_canonical_frames(
    OcrGenerationResult result,
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir) {
  CollectedOcrFrames collected;
  const int total = static_cast<int>(frame_input.frames.size());
  for (std::size_t index = 0; index < frame_input.frames.size(); ++index) {
    const ColorRasterFrame& frame = frame_input.frames[index];
    if (options.on_progress) {
      options.on_progress(static_cast<int>(index) + 1, total);
    }
    collect_ocr_sample(
        run_ocr_on_decoded_frame(pp_ocr_session, pp_ocr_opts, frame,
                                 static_cast<std::uint64_t>(index)),
        OcrSampleFrame{frame.frame_id, frame.frame_index}, collected);
  }
  collected.processed_frame_count = total;
  if (!frame_input.frames.empty()) {
    collected.processed_frame_width = frame_input.frames[0].width;
    collected.processed_frame_height = frame_input.frames[0].height;
  }
  result.ocr_frame_input_available =
      frame_input.decoding_succeeded && !frame_input.frames.empty();
  return complete_ocr_generation(std::move(result), options, collected,
                                 frame_input, pp_ocr_session, pp_ocr_opts,
                                 staging_dir);
}

void check_session_memory(const OcrGenerationOptions& options,
                          const PpOcrOptions& pp_ocr_opts,
                          const PpOcrSession& pp_ocr_session) {
  svp::core::check_memory_limit("ocr.generation.session_created", {
      {"available", pp_ocr_session.available ? "true" : "false"},
      {"blocker", pp_ocr_session.blocker},
      {"execution_provider", pp_ocr_opts.execution_provider},
      {"graph_optimization_level", std::to_string(pp_ocr_opts.graph_optimization_level)},
      {"execution_mode", pp_ocr_opts.execution_mode},
      {"det_intra_op_num_threads", std::to_string(pp_ocr_opts.det_threads.intra_op)},
      {"det_inter_op_num_threads", std::to_string(pp_ocr_opts.det_threads.inter_op)},
      {"det_graph_optimization_level", std::to_string(pp_ocr_opts.det_graph_optimization_level)},
      {"det_execution_mode", pp_ocr_opts.det_execution_mode},
      {"rec_intra_op_num_threads", std::to_string(pp_ocr_opts.rec_threads.intra_op)},
      {"rec_inter_op_num_threads", std::to_string(pp_ocr_opts.rec_threads.inter_op)},
      {"rec_graph_optimization_level", std::to_string(pp_ocr_opts.rec_graph_optimization_level)},
      {"rec_execution_mode", pp_ocr_opts.rec_execution_mode},
      {"performance_profile", options.performance_profile},
      {"recognition_parallel_workers", std::to_string(pp_ocr_opts.recognition_parallel_workers)},
      {"recognition_parallel_min_boxes", std::to_string(pp_ocr_opts.recognition_parallel_min_boxes)}
  });
}

}  // namespace

OcrSourceFrameDimensions derive_ocr_source_frame_dimensions(
    int stored_width,
    int stored_height,
    int rotation_degrees) {
  const int normalized_rotation = ((rotation_degrees % 360) + 360) % 360;
  const bool swaps_axes =
      normalized_rotation == 90 || normalized_rotation == 270;
  return swaps_axes
      ? OcrSourceFrameDimensions{stored_height, stored_width}
      : OcrSourceFrameDimensions{stored_width, stored_height};
}

OcrSourceFrameDimensions ocr_decode_frame_dimensions(
    const svp::media::MediaIngestPlan& media_plan) {
  int src_w = static_cast<int>(media_plan.primary_video_stream.width);
  int src_h = static_cast<int>(media_plan.primary_video_stream.height);
  if (std::abs(media_plan.primary_video_stream.rotation_degrees) == 90) {
    std::swap(src_w, src_h);
  }
  if (src_w <= kOcrMaxFrameDimension && src_h <= kOcrMaxFrameDimension) {
    return {src_w, src_h};
  }
  if (src_w >= src_h) {
    return {kOcrMaxFrameDimension,
            static_cast<int>(std::round(static_cast<double>(src_h) *
                                        kOcrMaxFrameDimension / src_w))};
  }
  return {static_cast<int>(std::round(static_cast<double>(src_w) *
                                      kOcrMaxFrameDimension / src_h)),
          kOcrMaxFrameDimension};
}

std::optional<OcrSamplePlan> plan_ocr_samples(
    const OcrGenerationOptions& options) {
  if (options.media_plan == nullptr ||
      options.ocr_frame_width <= 0 || options.ocr_frame_height <= 0) {
    return std::nullopt;
  }
  return make_ocr_sample_plan(
      plan_ocr_temporal_sampling(compute_media_duration_us(*options.media_plan),
                                 options.sampling_config,
                                 ocr_diagnostic_timestamp_override()),
      options.ocr_frame_width, options.ocr_frame_height);
}

OcrGenerationResult generate_ocr_observations(
    const OcrGenerationOptions& options,
    const DecodedCanonicalFrames& frame_input,
    const std::filesystem::path& staging_dir) {
  OcrGenerationResult result;

  const PpOcrOptions pp_ocr_opts = make_ocr_pp_ocr_options(options);
  PpOcrSession pp_ocr_session = create_pp_ocr_session(pp_ocr_opts);
  check_session_memory(options, pp_ocr_opts, pp_ocr_session);

  result.ocr_available = pp_ocr_session.available;
  result.ocr_frame_input_available =
      frame_input.decoding_succeeded && !frame_input.frames.empty();

  if (!result.ocr_available) {
    result.blocker = pp_ocr_session.blocker;
    return finish_ocr_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        "PP-OCR not available");
  }

  std::optional<OcrSamplePlan> plan = plan_ocr_samples(options);
  if (plan.has_value()) {
    result.temporal_sampling = plan->temporal_sampling;
  }
  const bool use_sample_plan = plan.has_value() && !plan->samples.empty();

  if (!use_sample_plan && !result.ocr_frame_input_available) {
    result.blocker = ocr_frame_input_blocker(frame_input);
    return finish_ocr_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        result.blocker);
  }

  result.ocr_detection_run = true;
  if (use_sample_plan) {
    return run_sample_plan_in_process(std::move(result), options, *plan,
                                      pp_ocr_session, pp_ocr_opts, staging_dir);
  }
  return run_canonical_frames(std::move(result), options, frame_input,
                              pp_ocr_session, pp_ocr_opts, staging_dir);
}

OcrGenerationResult reduce_ocr_frame_batches(
    const OcrGenerationOptions& options,
    const OcrSamplePlan& plan,
    std::vector<std::vector<OcrSampleDetections>> batch_results,
    const PpOcrSession& pp_ocr_session,
    const PpOcrOptions& pp_ocr_opts,
    const std::filesystem::path& staging_dir) {
  std::vector<OcrSampleDetections> samples =
      assemble_ocr_frame_batches(plan, std::move(batch_results));

  OcrGenerationResult result;
  result.ocr_available = pp_ocr_session.available;
  result.temporal_sampling = plan.temporal_sampling;
  if (!result.ocr_available) {
    result.blocker = pp_ocr_session.blocker;
    return finish_ocr_not_executed(
        std::move(result),
        staging_dir,
        result.blocker,
        "PP-OCR not available");
  }
  result.ocr_detection_run = true;
  return reduce_ordered_samples(std::move(result), options, samples,
                                pp_ocr_session, pp_ocr_opts, staging_dir);
}

DecodedCanonicalFrames decode_ocr_frames_temporal(
    const OcrGenerationOptions& options,
    OcrTemporalSamplingResult& out_sampling) {
  if (options.media_plan == nullptr ||
      options.ocr_frame_width <= 0 || options.ocr_frame_height <= 0) {
    return {};
  }

  const std::int64_t duration_us =
      compute_media_duration_us(*options.media_plan);

  out_sampling = compute_ocr_temporal_timestamps(duration_us, options.sampling_config);

  if (out_sampling.timestamps_us.empty()) {
    DecodedCanonicalFrames empty;
    empty.decoding_attempted = false;
    empty.skipped_reason = "no OCR temporal timestamps computed (duration unknown?)";
    return empty;
  }

  return decode_frames_at_timestamps(
      *options.media_plan,
      options.ffmpeg_path,
      options.ocr_frame_width,
      options.ocr_frame_height,
      out_sampling.timestamps_us,
      options.frame_catalog,
      kOcrFramePurpose);
}

}  // namespace svp::vision
