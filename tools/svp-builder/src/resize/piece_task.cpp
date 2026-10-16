#include "resize/piece_task.hpp"

#include "svp/exec/cache_key.hpp"
#include "svp/exec/parameters_digest.hpp"

#include <stdexcept>

namespace svp::builder::resize {
namespace {

constexpr std::string_view kPieceMediaType = "application/octet-stream";

[[noreturn]] void invalid(const std::string& why) {
  throw std::invalid_argument("resize.piece parameters: " + why);
}

const nlohmann::json& member(const nlohmann::json& object, std::string_view name) {
  const auto found = object.find(std::string(name));
  if (found == object.end()) {
    invalid("missing " + std::string(name));
  }
  return *found;
}

int positive_even_side(const nlohmann::json& value, std::string_view name) {
  if (!value.is_number_integer() || value.get<std::int64_t>() <= 0 ||
      value.get<std::int64_t>() % 2 != 0 || value.get<std::int64_t>() > INT32_MAX) {
    invalid(std::string(name) + " must be a positive even integer");
  }
  return static_cast<int>(value.get<std::int64_t>());
}

}  // namespace

nlohmann::json resize_piece_parameters_to_json(const ResizePieceParameters& value) {
  return nlohmann::json{{"encoding",
                         {{"stored_width", value.encoding.stored_width},
                          {"stored_height", value.encoding.stored_height},
                          {"time_base", value.encoding.time_base}}},
                        {"ffmpeg_build", value.ffmpeg_build}};
}

ResizePieceParameters resize_piece_parameters_from_json(const nlohmann::json& value) {
  if (!value.is_object() || value.size() != 2) {
    invalid("needs exactly encoding and ffmpeg_build");
  }
  const nlohmann::json& encoding = member(value, "encoding");
  if (!encoding.is_object() || encoding.size() != 3) {
    invalid("encoding needs exactly stored_width, stored_height and time_base");
  }
  const nlohmann::json& time_base = member(encoding, "time_base");
  const nlohmann::json& ffmpeg_build = member(value, "ffmpeg_build");
  if (!time_base.is_string()) {
    invalid("encoding.time_base must be a string");
  }
  const std::optional<svp::media::Rational> parsed =
      svp::media::parse_rational(time_base.get<std::string>());
  if (!parsed || parsed->numerator <= 0 || parsed->denominator <= 0 ||
      parsed->denominator > kMpeg4MaxTimeBaseDenominator) {
    invalid("encoding.time_base must be a time base MPEG-4 Part 2 can store");
  }
  if (!ffmpeg_build.is_string() || ffmpeg_build.get<std::string>().empty()) {
    invalid("ffmpeg_build must be a non-empty string");
  }
  return ResizePieceParameters{
      .encoding = PieceEncoding{.stored_width = positive_even_side(
                                    member(encoding, "stored_width"), "encoding.stored_width"),
                                .stored_height = positive_even_side(
                                    member(encoding, "stored_height"), "encoding.stored_height"),
                                .time_base = time_base.get<std::string>()},
      .ffmpeg_build = ffmpeg_build.get<std::string>()};
}

std::optional<std::string> validate_resize_piece_parameters(const nlohmann::json& value) {
  try {
    (void)resize_piece_parameters_from_json(value);
    return std::nullopt;
  } catch (const std::invalid_argument& error) {
    return std::string(error.what());
  }
}

svp::exec::TaskSpec make_resize_piece_spec(const std::string& build_session_id,
                                           const std::string& task_id,
                                           const svp::exec::ArtifactRef& piece,
                                           const ResizePieceParameters& parameters) {
  svp::exec::TaskSpec spec;
  spec.build_session_id = build_session_id;
  spec.task_id = task_id;
  spec.task_type = std::string(kResizePieceTaskType);
  spec.task_type_version = kResizePieceTaskTypeVersion;
  svp::exec::ArtifactRef input = piece;
  input.media_type = std::string(kPieceMediaType);
  input.role = std::string(kResizePieceInput);
  spec.inputs.emplace(std::string(kResizePieceInput), input);
  spec.parameters = resize_piece_parameters_to_json(parameters);
  spec.parameters_blake3 = svp::exec::compute_parameters_blake3(spec.parameters);
  spec.cache_key = svp::exec::compute_cache_key(nlohmann::json::array({
      spec.task_type,
      spec.task_type_version,
      svp::exec::blake3_hex(input.blake3),
      svp::exec::blake3_hex(spec.parameters_blake3),
  }));
  spec.resources = svp::exec::TaskResources{.est_peak_rss_mb = kResizePieceEstimatedPeakRssMb,
                                            .est_cpu_threads = kResizePieceEstimatedCpuThreads,
                                            .est_seconds = 0};
  svp::exec::validate_task_spec(spec);
  return spec;
}

std::string encode_encoded_piece_record(const EncodedPieceRecord& record) {
  return nlohmann::json{{"blob",
                         {{"blake3", svp::exec::blake3_hex(record.blake3)},
                          {"bytes", record.bytes}}}}
      .dump();
}

EncodedPieceRecord decode_encoded_piece_record(std::string_view text) {
  const nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
  if (!value.is_object() || value.size() != 1 || !value.contains("blob")) {
    throw std::invalid_argument("resize.piece record: needs exactly blob");
  }
  const nlohmann::json& blob = value.at("blob");
  if (!blob.is_object() || blob.size() != 2 || !blob.contains("blake3") ||
      !blob.contains("bytes") || !blob.at("blake3").is_string() ||
      !blob.at("bytes").is_number_unsigned()) {
    throw std::invalid_argument("resize.piece record: blob needs exactly blake3 and bytes");
  }
  const std::optional<svp::exec::Blake3Digest> digest =
      svp::exec::parse_blake3_hex(blob.at("blake3").get<std::string>());
  if (!digest) {
    throw std::invalid_argument("resize.piece record: blob.blake3 is not a BLAKE3 digest");
  }
  return EncodedPieceRecord{.blake3 = *digest, .bytes = blob.at("bytes").get<std::uint64_t>()};
}

}  // namespace svp::builder::resize
