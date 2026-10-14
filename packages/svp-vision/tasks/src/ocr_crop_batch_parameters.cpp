#include "svp/vision/tasks/ocr_crop_batch_parameters.hpp"

#include "parameter_fields.hpp"
#include "svp/vision/tasks/pp_ocr_parameters.hpp"

#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace svp::vision::tasks {
namespace {

using detail::Json;

const detail::ParameterFields& fields() {
  static const detail::ParameterFields kFields(kOcrCropBatchTaskType);
  return kFields;
}

constexpr std::array<std::string_view, 2> kImageFormats = {kEvidenceCropFormatJpeg,
                                                           kEvidenceCropFormatPng};

EvidenceCropJob job_from_json(const Json& value, const std::string& where) {
  // The frame size is optional, as a pair (a working copy's crop).
  const bool has_frame = value.is_object() && value.contains("frame_width");
  std::set<std::string> expected{"height", "image_format", "jpeg_quality", "left",
                                 "ordinal", "seek_us", "top", "width"};
  if (has_frame) {
    expected.insert({"frame_height", "frame_width"});
  }
  fields().require_fields(value, expected, where);
  EvidenceCropJob job{
      .ordinal = fields().integer_at<std::uint64_t>(value, "ordinal", where, 0),
      .seek_us = fields().integer_at<std::int64_t>(value, "seek_us", where, 0),
      .left = fields().integer_at<int>(value, "left", where, 0),
      .top = fields().integer_at<int>(value, "top", where, 0),
      .width = fields().integer_at<int>(value, "width", where, 1),
      .height = fields().integer_at<int>(value, "height", where, 1),
      .image_format = fields().one_of(value, "image_format", where, kImageFormats),
      .jpeg_quality = fields().integer_at<int>(value, "jpeg_quality", where, 1, kMaxJpegQuality),
  };
  if (has_frame) {
    job.frame_width = fields().integer_at<int>(value, "frame_width", where, 1);
    job.frame_height = fields().integer_at<int>(value, "frame_height", where, 1);
    if (job.left > job.frame_width - job.width || job.top > job.frame_height - job.height) {
      fields().reject(where + " rectangle does not fit its frame");
    }
  }
  return job;
}

Json job_to_json(const EvidenceCropJob& job) {
  Json value{{"height", job.height},       {"image_format", job.image_format},
             {"jpeg_quality", job.jpeg_quality}, {"left", job.left},
             {"ordinal", job.ordinal},     {"seek_us", job.seek_us},
             {"top", job.top},             {"width", job.width}};
  if (job.frame_width != 0 || job.frame_height != 0) {
    value["frame_width"] = job.frame_width;
    value["frame_height"] = job.frame_height;
  }
  return value;
}

}  // namespace

Json ocr_crop_batch_parameters_to_json(const OcrCropBatchParameters& parameters) {
  Json jobs = Json::array();
  for (const EvidenceCropJob& job : parameters.jobs) {
    jobs.push_back(job_to_json(job));
  }
  Json value = pp_ocr_parameter_fields(parameters.pp_ocr);
  value["decode"] = {{"ffmpeg_build", parameters.ffmpeg_build}};
  value["jobs"] = std::move(jobs);
  // One set of rules for both directions.
  (void)ocr_crop_batch_parameters_from_json(value);
  return value;
}

OcrCropBatchParameters ocr_crop_batch_parameters_from_json(const Json& value) {
  fields().require_fields(value,
                          {"decode", "detector", "execution_provider", "jobs", "recognizer"},
                          "parameters");
  OcrCropBatchParameters parameters;
  const Json& jobs = fields().array_at(value, "jobs", "parameters");
  if (jobs.empty()) fields().reject("jobs must not be empty");
  for (std::size_t index = 0; index < jobs.size(); ++index) {
    const std::string where = "jobs[" + std::to_string(index) + "]";
    EvidenceCropJob job = job_from_json(jobs[index], where);
    if (!parameters.jobs.empty() && job.ordinal <= parameters.jobs.back().ordinal) {
      fields().reject(where + " is not strictly after the job before it");
    }
    parameters.jobs.push_back(std::move(job));
  }
  const Json& decode = value.at("decode");
  fields().require_fields(decode, {"ffmpeg_build"}, "decode");
  parameters.ffmpeg_build = fields().ffmpeg_build_at(decode, "decode");
  parameters.pp_ocr = pp_ocr_options_from_parameters(value, kOcrCropBatchTaskType);
  return parameters;
}

std::optional<std::string> validate_ocr_crop_batch_parameters(const Json& value) {
  try {
    (void)ocr_crop_batch_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string(kOcrCropBatchTaskType) + " parameters: " + error.what();
  }
}

}  // namespace svp::vision::tasks
