#include "svp/vision/visual_entity_window_codec.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace svp::vision {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
  throw VisualEntityWindowCodecError("visual entity window outcome: " + message);
}

constexpr std::array<std::pair<VisualEntityWindowStatus, std::string_view>, 4> kStatuses{{
    {VisualEntityWindowStatus::decode_failed, "decode_failed"},
    {VisualEntityWindowStatus::tracker_failed, "tracker_failed"},
    {VisualEntityWindowStatus::tracked, "tracked"},
    {VisualEntityWindowStatus::not_started, "not_started"},
}};

std::string status_name(VisualEntityWindowStatus status) {
  for (const auto& [value, name] : kStatuses) {
    if (value == status) return std::string(name);
  }
  fail("unknown status");
}

VisualEntityWindowStatus parse_status(const std::string& name) {
  for (const auto& [value, known] : kStatuses) {
    if (known == name) return value;
  }
  fail("unknown status `" + name + "`");
}

Json binary(std::vector<std::uint8_t> bytes) { return Json::binary(std::move(bytes)); }

const std::vector<std::uint8_t>& binary_of(const Json& value, std::string_view what) {
  if (!value.is_binary()) fail(std::string(what) + " must be a byte string");
  return value.get_binary();
}

// Little-endian IEEE-754 binary32, independent of the host's byte order.
std::vector<std::uint8_t> encode_floats(const std::vector<float>& values) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(values.size() * sizeof(std::uint32_t));
  for (const float value : values) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
  }
  return bytes;
}

std::vector<float> decode_floats(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() % sizeof(std::uint32_t) != 0) fail("embedding bytes are not binary32 values");
  std::vector<float> values;
  values.reserve(bytes.size() / sizeof(std::uint32_t));
  for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint32_t)) {
    std::uint32_t bits = 0;
    for (int byte = 0; byte < 4; ++byte) {
      bits |= static_cast<std::uint32_t>(bytes[offset + byte]) << (8 * byte);
    }
    values.push_back(std::bit_cast<float>(bits));
  }
  return values;
}

template <typename T, std::size_t N>
Json array_of(const T (&values)[N]) {
  Json out = Json::array();
  for (const T& value : values) out.push_back(value);
  return out;
}

template <typename T, std::size_t N>
void read_array(const Json& value, T (&out)[N], std::string_view what) {
  if (!value.is_array() || value.size() != N) {
    fail(std::string(what) + " must be an array of " + std::to_string(N));
  }
  for (std::size_t index = 0; index < N; ++index) out[index] = value[index].get<T>();
}

Json encode_region(const TrackedRegion& region) {
  Json mask = nullptr;
  if (!region.mask_pixels.empty()) {
    const std::size_t total =
        static_cast<std::size_t>(std::max(region.mask_width, 0)) *
        static_cast<std::size_t>(std::max(region.mask_height, 0));
    if (total == 0 || region.mask_pixels.size() != total) {
      fail("region " + region.region_id + " mask does not cover its width x height");
    }
    for (const std::uint8_t pixel : region.mask_pixels) {
      if (pixel > 1) fail("region " + region.region_id + " mask is not binary");
    }
    mask = binary(encode_mask_rle(region.mask_pixels.data(), region.mask_width,
                                  region.mask_height));
  }
  return {{"box_norm", array_of(region.box_norm)},
          {"box_px", array_of(region.box_px)},
          {"candidate_source", region.candidate_source},
          {"centroid_norm", array_of(region.centroid_norm)},
          {"confidence", region.confidence},
          {"depth_ref", region.depth_ref},
          {"detector_category_index", region.detector_category_index},
          {"embedding", binary(encode_floats(region.embedding))},
          {"embedding_model_id", region.embedding_model_id},
          {"entity_id", region.entity_id},
          {"far_percentile_90", region.far_percentile_90},
          {"frame_id", region.frame_id},
          {"mask_height", region.mask_height},
          {"mask_ref", region.mask_ref},
          {"mask_rle", std::move(mask)},
          {"mask_width", region.mask_width},
          {"median_inverse_depth", region.median_inverse_depth},
          {"near_percentile_10", region.near_percentile_10},
          {"region_id", region.region_id},
          {"screen_area_ratio", region.screen_area_ratio},
          {"timestamp_us", region.timestamp_us},
          {"track_id", region.track_id}};
}

TrackedRegion decode_region(const Json& value) {
  TrackedRegion region;
  read_array(value.at("box_norm"), region.box_norm, "box_norm");
  read_array(value.at("box_px"), region.box_px, "box_px");
  region.candidate_source = value.at("candidate_source").get<std::string>();
  read_array(value.at("centroid_norm"), region.centroid_norm, "centroid_norm");
  region.confidence = value.at("confidence").get<double>();
  region.depth_ref = value.at("depth_ref").get<std::string>();
  region.detector_category_index = value.at("detector_category_index").get<int>();
  region.embedding = decode_floats(binary_of(value.at("embedding"), "embedding"));
  region.embedding_model_id = value.at("embedding_model_id").get<std::string>();
  region.entity_id = value.at("entity_id").get<std::string>();
  region.far_percentile_90 = value.at("far_percentile_90").get<double>();
  region.frame_id = value.at("frame_id").get<std::string>();
  region.mask_height = value.at("mask_height").get<int>();
  region.mask_ref = value.at("mask_ref").get<std::string>();
  region.mask_width = value.at("mask_width").get<int>();
  region.median_inverse_depth = value.at("median_inverse_depth").get<double>();
  region.near_percentile_10 = value.at("near_percentile_10").get<double>();
  region.region_id = value.at("region_id").get<std::string>();
  region.screen_area_ratio = value.at("screen_area_ratio").get<double>();
  region.timestamp_us = value.at("timestamp_us").get<std::int64_t>();
  region.track_id = value.at("track_id").get<std::string>();
  const Json& mask = value.at("mask_rle");
  if (!mask.is_null()) {
    const auto& rle = binary_of(mask, "mask_rle");
    region.mask_pixels =
        decode_mask_rle(rle.data(), rle.size(), region.mask_width, region.mask_height);
    if (region.mask_pixels.empty()) fail("region " + region.region_id + " mask RLE is invalid");
  }
  return region;
}

Json encode_entity(const EntityRecord& entity) {
  return {{"average_screen_area", entity.average_screen_area},
          {"average_visibility", entity.average_visibility},
          {"entity_id", entity.entity_id},
          {"entity_type", entity.entity_type},
          {"evidence_sources", entity.evidence_sources},
          {"first_seen_us", entity.first_seen_us},
          {"last_seen_us", entity.last_seen_us},
          {"processor_id", entity.processor_id},
          {"track_ids", entity.track_ids}};
}

EntityRecord decode_entity(const Json& value) {
  EntityRecord entity;
  entity.average_screen_area = value.at("average_screen_area").get<double>();
  entity.average_visibility = value.at("average_visibility").get<double>();
  entity.entity_id = value.at("entity_id").get<std::string>();
  entity.entity_type = value.at("entity_type").get<std::string>();
  entity.evidence_sources = value.at("evidence_sources").get<std::vector<Json>>();
  entity.first_seen_us = value.at("first_seen_us").get<std::int64_t>();
  entity.last_seen_us = value.at("last_seen_us").get<std::int64_t>();
  entity.processor_id = value.at("processor_id").get<std::string>();
  entity.track_ids = value.at("track_ids").get<std::vector<std::string>>();
  return entity;
}

Json encode_track(const TrackRecord& track) {
  return {{"candidate_source", track.candidate_source},
          {"confidence", track.confidence},
          {"end_frame_id", track.end_frame_id},
          {"end_us", track.end_us},
          {"entity_id", track.entity_id},
          {"lost_frame_count", track.lost_frame_count},
          {"processor_id", track.processor_id},
          {"reacquired", track.reacquired},
          {"region_count", track.region_count},
          {"start_frame_id", track.start_frame_id},
          {"start_us", track.start_us},
          {"track_id", track.track_id},
          {"tracking_method", track.tracking_method}};
}

TrackRecord decode_track(const Json& value) {
  TrackRecord track;
  track.candidate_source = value.at("candidate_source").get<std::string>();
  track.confidence = value.at("confidence").get<double>();
  track.end_frame_id = value.at("end_frame_id").get<std::string>();
  track.end_us = value.at("end_us").get<std::int64_t>();
  track.entity_id = value.at("entity_id").get<std::string>();
  track.lost_frame_count = value.at("lost_frame_count").get<int>();
  track.processor_id = value.at("processor_id").get<std::string>();
  track.reacquired = value.at("reacquired").get<bool>();
  track.region_count = value.at("region_count").get<int>();
  track.start_frame_id = value.at("start_frame_id").get<std::string>();
  track.start_us = value.at("start_us").get<std::int64_t>();
  track.track_id = value.at("track_id").get<std::string>();
  track.tracking_method = value.at("tracking_method").get<std::string>();
  return track;
}

Json encode_tracker(const EntityTrackResult& result) {
  Json regions = Json::array();
  for (const auto& region : result.regions) regions.push_back(encode_region(region));
  Json entities = Json::array();
  for (const auto& entity : result.entities) entities.push_back(encode_entity(entity));
  Json tracks = Json::array();
  for (const auto& track : result.tracks) tracks.push_back(encode_track(track));
  return {{"confidence_calibration_status", result.confidence_calibration_status},
          {"entities", std::move(entities)},
          {"execution_provider", result.execution_provider},
          {"limitations_note", result.limitations_note},
          {"model_refs", result.model_refs},
          {"opencv_version", result.opencv_version},
          {"parameters_json", result.parameters_json},
          {"processing_status", result.processing_status},
          {"processor_id", result.processor_id},
          {"regions", std::move(regions)},
          {"runtime", result.runtime},
          {"tracks", std::move(tracks)}};
}

EntityTrackResult decode_tracker(const Json& value) {
  EntityTrackResult result;
  result.confidence_calibration_status =
      value.at("confidence_calibration_status").get<std::string>();
  for (const Json& entity : value.at("entities")) result.entities.push_back(decode_entity(entity));
  result.execution_provider = value.at("execution_provider").get<std::string>();
  result.limitations_note = value.at("limitations_note").get<std::string>();
  result.model_refs = value.at("model_refs").get<std::vector<std::string>>();
  result.opencv_version = value.at("opencv_version").get<std::string>();
  result.parameters_json = value.at("parameters_json");
  result.processing_status = value.at("processing_status").get<std::string>();
  result.processor_id = value.at("processor_id").get<std::string>();
  for (const Json& region : value.at("regions")) result.regions.push_back(decode_region(region));
  result.runtime = value.at("runtime").get<std::string>();
  for (const Json& track : value.at("tracks")) result.tracks.push_back(decode_track(track));
  return result;
}

Json encode_diagnostics(const VisualEntityDetectorDiagnostics& diagnostics) {
  return {{"area_filtered", diagnostics.area_filtered},
          {"cap_filtered", diagnostics.cap_filtered},
          {"confidence_filtered", diagnostics.confidence_filtered},
          {"detections_emitted", diagnostics.detections_emitted},
          {"duplicate_filtered", diagnostics.duplicate_filtered},
          {"queries_evaluated", diagnostics.queries_evaluated}};
}

VisualEntityDetectorDiagnostics decode_diagnostics(const Json& value) {
  return {.queries_evaluated = value.at("queries_evaluated").get<std::size_t>(),
          .confidence_filtered = value.at("confidence_filtered").get<std::size_t>(),
          .area_filtered = value.at("area_filtered").get<std::size_t>(),
          .duplicate_filtered = value.at("duplicate_filtered").get<std::size_t>(),
          .cap_filtered = value.at("cap_filtered").get<std::size_t>(),
          .detections_emitted = value.at("detections_emitted").get<std::size_t>()};
}

Json encode_runtime_status(const VisualEntityWindowRuntimeStatus& status) {
  return {{"depth_blocker", status.depth_blocker},
          {"depth_loaded", status.depth_loaded},
          {"detector_blocker", status.detector_blocker},
          {"detector_loaded", status.detector_loaded},
          {"detector_model_identity", status.detector_model_identity},
          {"detector_model_refs", status.detector_model_refs}};
}

VisualEntityWindowRuntimeStatus decode_runtime_status(const Json& value) {
  return {.detector_loaded = value.at("detector_loaded").get<bool>(),
          .detector_blocker = value.at("detector_blocker").get<std::string>(),
          .detector_model_refs = value.at("detector_model_refs").get<std::vector<std::string>>(),
          .detector_model_identity = value.at("detector_model_identity"),
          .depth_loaded = value.at("depth_loaded").get<bool>(),
          .depth_blocker = value.at("depth_blocker").get<std::string>()};
}

}  // namespace

std::vector<std::uint8_t> encode_visual_entity_window_outcome(
    const VisualEntityWindowOutcome& outcome) {
  Json failures = Json::array();
  for (const auto& failure : outcome.failures) {
    failures.push_back(Json::array({failure.component, failure.message}));
  }
  Json cuts = Json::array();
  for (const auto& evidence : outcome.cut_evidence) {
    cuts.push_back({{"difference", evidence.difference},
                    {"immediate_following_difference", evidence.immediate_following_difference},
                    {"is_cut", evidence.is_cut},
                    {"is_sustained_transition", evidence.is_sustained_transition},
                    {"minimum_lookahead_difference", evidence.minimum_lookahead_difference},
                    {"timestamp_us", evidence.timestamp_us}});
  }
  Json value = {
      {"version", kVisualEntityWindowCodecVersion},
      {"status", status_name(outcome.status)},
      {"frames_attempted", outcome.frames_attempted},
      {"frames_decoded", outcome.frames_decoded},
      {"frames_missed", outcome.frames_missed},
      {"decoded_timestamps_us", outcome.decoded_timestamps_us},
      {"failures", std::move(failures)},
      {"cut_evidence", std::move(cuts)},
      {"cut_timestamps_us", outcome.cut_timestamps_us},
      {"detector_diagnostics", encode_diagnostics(outcome.detector_diagnostics)},
      {"runtime_status", encode_runtime_status(outcome.runtime_status)},
      {"tracker", outcome.status == VisualEntityWindowStatus::tracked
                      ? encode_tracker(outcome.tracker_result)
                      : Json(nullptr)}};
  return Json::to_cbor(value);
}

VisualEntityWindowHeader peek_visual_entity_window_header(std::span<const std::uint8_t> bytes) {
  try {
    const Json value = Json::from_cbor(bytes.begin(), bytes.end());
    if (value.at("version").get<std::uint64_t>() != kVisualEntityWindowCodecVersion) {
      fail("unsupported version");
    }
    return VisualEntityWindowHeader{
        .status = parse_status(value.at("status").get<std::string>()),
        .runtime_status = decode_runtime_status(value.at("runtime_status"))};
  } catch (const VisualEntityWindowCodecError&) {
    throw;
  } catch (const std::exception& error) {
    fail(error.what());
  }
}

VisualEntityWindowOutcome decode_visual_entity_window_outcome(
    std::span<const std::uint8_t> bytes) {
  try {
    const Json value = Json::from_cbor(bytes.begin(), bytes.end());
    if (value.at("version").get<std::uint64_t>() != kVisualEntityWindowCodecVersion) {
      fail("unsupported version");
    }
    VisualEntityWindowOutcome outcome;
    outcome.status = parse_status(value.at("status").get<std::string>());
    outcome.frames_attempted = value.at("frames_attempted").get<std::size_t>();
    outcome.frames_decoded = value.at("frames_decoded").get<std::size_t>();
    outcome.frames_missed = value.at("frames_missed").get<std::size_t>();
    outcome.decoded_timestamps_us =
        value.at("decoded_timestamps_us").get<std::vector<std::int64_t>>();
    for (const Json& failure : value.at("failures")) {
      if (!failure.is_array() || failure.size() != 2) {
        fail("a failure must be [component, message]");
      }
      outcome.failures.push_back(
          {failure[0].get<std::string>(), failure[1].get<std::string>()});
    }
    for (const Json& cut : value.at("cut_evidence")) {
      outcome.cut_evidence.push_back(VisualEntityCutEvidence{
          .timestamp_us = cut.at("timestamp_us").get<std::int64_t>(),
          .difference = cut.at("difference").get<double>(),
          .immediate_following_difference =
              cut.at("immediate_following_difference").get<double>(),
          .minimum_lookahead_difference = cut.at("minimum_lookahead_difference").get<double>(),
          .is_sustained_transition = cut.at("is_sustained_transition").get<bool>(),
          .is_cut = cut.at("is_cut").get<bool>()});
    }
    outcome.cut_timestamps_us = value.at("cut_timestamps_us").get<std::vector<std::int64_t>>();
    outcome.detector_diagnostics = decode_diagnostics(value.at("detector_diagnostics"));
    outcome.runtime_status = decode_runtime_status(value.at("runtime_status"));
    const Json& tracker = value.at("tracker");
    if ((outcome.status == VisualEntityWindowStatus::tracked) == tracker.is_null()) {
      fail("tracker output must be present exactly for a tracked window");
    }
    if (!tracker.is_null()) outcome.tracker_result = decode_tracker(tracker);
    return outcome;
  } catch (const VisualEntityWindowCodecError&) {
    throw;
  } catch (const std::exception& error) {
    fail(error.what());
  }
}

}  // namespace svp::vision
