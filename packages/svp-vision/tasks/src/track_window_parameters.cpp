#include "svp/vision/tasks/track_window_parameters.hpp"

#include "parameter_fields.hpp"

#include "svp/models/reference_processor_model_ids.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>

namespace svp::vision::tasks {
namespace {

using Json = nlohmann::json;

using detail::kExecutionProviders;
using detail::threads_to_json;
using Reader = detail::ParameterFields;

constexpr double kUnbounded = std::numeric_limits<double>::max();

void read_window(const Reader& read, const Json& value,
                 TrackWindowParameters& parameters) {
  const std::string where = "window";
  read.require_fields(value,
                      {"end_us", "frame_ids", "frame_indices", "ordinal", "start_us",
                       "timestamps_us"},
                      where);
  parameters.window_ordinal = read.integer_at<std::uint64_t>(value, "ordinal", where, 0);
  parameters.window.start_us = read.integer_at<std::int64_t>(value, "start_us", where, 0);
  parameters.window.end_us =
      read.integer_at<std::int64_t>(value, "end_us", where, parameters.window.start_us);
  const Json& timestamps = value.at("timestamps_us");
  const Json& ids = value.at("frame_ids");
  const Json& indices = value.at("frame_indices");
  if (!timestamps.is_array() || !ids.is_array() || !indices.is_array()) {
    read.reject("window.timestamps_us, frame_ids, and frame_indices must be arrays");
  }
  if (timestamps.empty()) read.reject("window.timestamps_us must not be empty");
  if (ids.size() != timestamps.size() || indices.size() != timestamps.size()) {
    read.reject("window.frame_ids and frame_indices must pair with window.timestamps_us");
  }
  for (std::size_t index = 0; index < timestamps.size(); ++index) {
    const std::string item = where + "[" + std::to_string(index) + "]";
    const Json entry{{"frame_id", ids[index]},
                     {"frame_index", indices[index]},
                     {"timestamp_us", timestamps[index]}};
    const auto timestamp = read.integer_at<std::int64_t>(entry, "timestamp_us", item,
                                                      parameters.window.start_us,
                                                      parameters.window.end_us);
    if (!parameters.window.timestamps_us.empty() &&
        timestamp <= parameters.window.timestamps_us.back()) {
      read.reject(item + " is not strictly after the timestamp before it");
    }
    const std::string frame_id = read.string_at(entry, "frame_id", item);
    if (frame_id.empty()) read.reject(item + ".frame_id must not be empty");
    parameters.window.timestamps_us.push_back(timestamp);
    parameters.frame_ids.push_back(frame_id);
    parameters.frame_indices.push_back(
        read.integer_at<std::uint64_t>(entry, "frame_index", item, 0));
  }
}

void read_detector(const Reader& read, const Json& value,
                   VisualEntityDetectorOptions& detector) {
  const std::string where = "detector";
  read.require_fields(value,
                      {"category_evidence_confidence_threshold", "confidence_threshold",
                       "cross_category_duplicate_iou_threshold", "maximum_area_ratio",
                       "maximum_detections", "minimum_area_ratio", "model_id",
                       "nms_containment_threshold", "nms_iou_threshold", "threads"},
                      where);
  detector.model_id = read.model_id_at(value, where);
  detector.threads = read.threads_at(value, where);
  detector.confidence_threshold = read.double_at(value, "confidence_threshold", where, 0.0, 1.0);
  detector.category_evidence_confidence_threshold =
      read.double_at(value, "category_evidence_confidence_threshold", where, 0.0, 1.0);
  detector.nms_iou_threshold = read.double_at(value, "nms_iou_threshold", where, 0.0, 1.0);
  detector.nms_containment_threshold =
      read.double_at(value, "nms_containment_threshold", where, 0.0, 1.0);
  detector.cross_category_duplicate_iou_threshold =
      read.double_at(value, "cross_category_duplicate_iou_threshold", where, 0.0, 1.0);
  detector.minimum_area_ratio = read.double_at(value, "minimum_area_ratio", where, 0.0, 1.0);
  detector.maximum_area_ratio = read.double_at(value, "maximum_area_ratio", where, 0.0, 1.0);
  detector.maximum_detections =
      read.integer_at<std::size_t>(value, "maximum_detections", where, 1);
}

}  // namespace

std::string track_window_depth_model_id() {
  return svp::models::kDepthAnythingV2SmallModelId;
}

VisualEntityPipelineOptions track_window_pipeline_options(
    const TrackWindowParameters& parameters) {
  VisualEntityPipelineOptions options;
  options.depth_schedule = parameters.depth_schedule;
  options.cut_detection = parameters.cut_detection;
  options.detector = parameters.detector;
  options.embedding_model_id = parameters.embedding_model_id;
  options.execution_provider = parameters.execution_provider;
  options.depth_threads = parameters.depth_threads;
  options.embedding_threads = parameters.embedding_threads;
  return options;
}

VisualEntityWindowRequest track_window_request(const TrackWindowParameters& parameters,
                                               const std::filesystem::path& source_path,
                                               const std::filesystem::path& ffmpeg_path) {
  return {.source_path = source_path,
          .ffmpeg_path = ffmpeg_path,
          .frame_width = parameters.frame_width,
          .frame_height = parameters.frame_height,
          .window = parameters.window,
          .depth_schedule = parameters.depth_schedule,
          .cut_detection = parameters.cut_detection,
          .embedding_model_id = parameters.embedding_model_id,
          .execution_provider = parameters.execution_provider};
}

Json track_window_parameters_to_json(const TrackWindowParameters& parameters) {
  const auto& cuts = parameters.cut_detection;
  const auto& detector = parameters.detector;
  const auto& schedule = parameters.depth_schedule;
  Json value{
      {"cut_detection",
       {{"extended_stability_lookahead_frames", cuts.extended_stability_lookahead_frames},
        {"hard_cut_difference_threshold", cuts.hard_cut_difference_threshold},
        {"immediate_difference_threshold", cuts.immediate_difference_threshold},
        {"stable_difference_threshold", cuts.stable_difference_threshold},
        {"sustained_transition_difference_threshold",
         cuts.sustained_transition_difference_threshold},
        {"sustained_transition_observations", cuts.sustained_transition_observations}}},
      {"decode", {{"ffmpeg_build", parameters.ffmpeg_build},
                  {"frame_height", parameters.frame_height},
                  {"frame_width", parameters.frame_width}}},
      {"depth", {{"model_id", track_window_depth_model_id()},
                 {"threads", threads_to_json(parameters.depth_threads)}}},
      {"depth_schedule",
       {{"periodic_interval_us", schedule.periodic_interval_us},
        {"scene_change_burst_frames", schedule.scene_change_burst_frames},
        {"scene_change_threshold", schedule.scene_change_threshold}}},
      {"detector",
       {{"category_evidence_confidence_threshold",
         detector.category_evidence_confidence_threshold},
        {"confidence_threshold", detector.confidence_threshold},
        {"cross_category_duplicate_iou_threshold",
         detector.cross_category_duplicate_iou_threshold},
        {"maximum_area_ratio", detector.maximum_area_ratio},
        {"maximum_detections", detector.maximum_detections},
        {"minimum_area_ratio", detector.minimum_area_ratio},
        {"model_id", detector.model_id},
        {"nms_containment_threshold", detector.nms_containment_threshold},
        {"nms_iou_threshold", detector.nms_iou_threshold},
        {"threads", threads_to_json(detector.threads)}}},
      {"embedding", {{"model_id", parameters.embedding_model_id},
                     {"threads", threads_to_json(parameters.embedding_threads)}}},
      {"execution_provider", parameters.execution_provider},
      {"window", {{"end_us", parameters.window.end_us},
                  {"frame_ids", parameters.frame_ids},
                  {"frame_indices", parameters.frame_indices},
                  {"ordinal", parameters.window_ordinal},
                  {"start_us", parameters.window.start_us},
                  {"timestamps_us", parameters.window.timestamps_us}}},
  };
  // One set of rules for both directions.
  (void)track_window_parameters_from_json(value);
  return value;
}

TrackWindowParameters track_window_parameters_from_json(const Json& value) {
  const Reader read(kTrackWindowTaskType);
  read.require_fields(value,
                      {"cut_detection", "decode", "depth", "depth_schedule", "detector",
                       "embedding", "execution_provider", "window"},
                      "parameters");
  TrackWindowParameters parameters;
  read_window(read, value.at("window"), parameters);

  const Json& decode = value.at("decode");
  read.require_fields(decode, {"ffmpeg_build", "frame_height", "frame_width"}, "decode");
  parameters.ffmpeg_build = read.ffmpeg_build_at(decode, "decode");
  parameters.frame_width = read.integer_at<int>(decode, "frame_width", "decode", 1,
                                             svp::media::kCanonicalLongestDisplayDimension);
  parameters.frame_height = read.integer_at<int>(decode, "frame_height", "decode", 1,
                                              svp::media::kCanonicalLongestDisplayDimension);

  parameters.execution_provider =
      read.one_of(value, "execution_provider", "parameters", kExecutionProviders);
  read_detector(read, value.at("detector"), parameters.detector);
  parameters.detector.execution_provider = parameters.execution_provider;

  const Json& depth = value.at("depth");
  read.require_fields(depth, {"model_id", "threads"}, "depth");
  if (read.model_id_at(depth, "depth") != track_window_depth_model_id()) {
    read.reject("depth.model_id must be " + track_window_depth_model_id() +
                ", the model a window loads");
  }
  parameters.depth_threads = read.threads_at(depth, "depth");

  const Json& embedding = value.at("embedding");
  read.require_fields(embedding, {"model_id", "threads"}, "embedding");
  parameters.embedding_model_id = read.model_id_at(embedding, "embedding");
  parameters.embedding_threads = read.threads_at(embedding, "embedding");

  const Json& schedule = value.at("depth_schedule");
  read.require_fields(schedule,
                      {"periodic_interval_us", "scene_change_burst_frames",
                       "scene_change_threshold"},
                      "depth_schedule");
  parameters.depth_schedule.periodic_interval_us =
      read.integer_at<std::int64_t>(schedule, "periodic_interval_us", "depth_schedule", 1);
  parameters.depth_schedule.scene_change_burst_frames =
      read.integer_at<std::size_t>(schedule, "scene_change_burst_frames", "depth_schedule", 0);
  parameters.depth_schedule.scene_change_threshold =
      read.double_at(schedule, "scene_change_threshold", "depth_schedule", 0.0, kUnbounded);

  const Json& cuts = value.at("cut_detection");
  const std::string cuts_where = "cut_detection";
  read.require_fields(cuts,
                      {"extended_stability_lookahead_frames", "hard_cut_difference_threshold",
                       "immediate_difference_threshold", "stable_difference_threshold",
                       "sustained_transition_difference_threshold",
                       "sustained_transition_observations"},
                      cuts_where);
  auto& cut = parameters.cut_detection;
  cut.extended_stability_lookahead_frames =
      read.integer_at<std::size_t>(cuts, "extended_stability_lookahead_frames", cuts_where, 0);
  cut.hard_cut_difference_threshold =
      read.double_at(cuts, "hard_cut_difference_threshold", cuts_where, 0.0, kUnbounded);
  cut.immediate_difference_threshold =
      read.double_at(cuts, "immediate_difference_threshold", cuts_where, 0.0, kUnbounded);
  cut.stable_difference_threshold =
      read.double_at(cuts, "stable_difference_threshold", cuts_where, 0.0, kUnbounded);
  cut.sustained_transition_difference_threshold =
      read.double_at(cuts, "sustained_transition_difference_threshold", cuts_where, 0.0, kUnbounded);
  cut.sustained_transition_observations =
      read.integer_at<std::size_t>(cuts, "sustained_transition_observations", cuts_where, 0);
  return parameters;
}

std::optional<std::string> validate_track_window_parameters(const Json& value) {
  try {
    (void)track_window_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  } catch (const Json::exception& error) {
    return std::string("track.window parameters: ") + error.what();
  }
}

}  // namespace svp::vision::tasks
