#include "svp/media/canonical_raster.hpp"
#include "svp/media/canonical_timing.hpp"
#include "svp/media/media_ingest_plan.hpp"
#include "svp/media/media_probe.hpp"

#include <cassert>

namespace {

void test_round_half_to_even() {
  assert(svp::media::round_half_to_even(1, 2) == 0);
  assert(svp::media::round_half_to_even(3, 2) == 2);
  assert(svp::media::round_half_to_even(5, 2) == 2);
  assert(svp::media::round_half_to_even(7, 2) == 4);
}

void test_frame_rates() {
  assert(svp::media::frame_index_to_microseconds(1, {24000, 1001}) == 41708);
  assert(svp::media::frame_index_to_microseconds(1, {30000, 1001}) == 33367);
  assert(svp::media::frame_index_to_microseconds(1, {25, 1}) == 40000);
  assert(svp::media::frame_index_to_microseconds(1, {30, 1}) == 33333);
  assert(svp::media::frame_index_to_microseconds(2, {30, 1}) == 66667);
  assert(svp::media::frame_index_to_microseconds(1, {60, 1}) == 16667);
}

void test_stream_pts_are_normalized_against_primary_presentation_start() {
  assert(svp::media::pts_delta_to_microseconds(
             96000, {1, 48000}, 1000, {1, 1000}) == 1000000);
  assert(svp::media::pts_delta_to_microseconds(
             24000, {1, 48000}, 1000, {1, 1000}) == -500000);
  assert(svp::media::normalized_pts_to_microseconds(
             96000, {1, 48000}, 1000, {1, 1000}) == 1000000);
}

void test_canonical_rasters() {
  auto landscape =
      svp::media::compute_canonical_analysis_raster({1920, 1080, 0, {1, 1}});
  assert(landscape.width == 640);
  assert(landscape.height == 360);

  auto vertical =
      svp::media::compute_canonical_analysis_raster({1080, 1920, 0, {1, 1}});
  assert(vertical.width == 360);
  assert(vertical.height == 640);

  auto square =
      svp::media::compute_canonical_analysis_raster({1080, 1080, 0, {1, 1}});
  assert(square.width == 640);
  assert(square.height == 640);

  auto scope =
      svp::media::compute_canonical_analysis_raster({2048, 858, 0, {1, 1}});
  assert(scope.width == 640);
  assert(scope.height == 268);

  auto rotated =
      svp::media::compute_canonical_analysis_raster({1920, 1080, 90, {1, 1}});
  assert(rotated.width == 360);
  assert(rotated.height == 640);
}

void test_probe_json_round_trip_preserves_nested_timing() {
  svp::media::MediaProbe probe;
  probe.format_name = "mov,mp4,m4a,3gp,3g2,mj2";
  probe.container_timing = svp::media::StreamTiming{
      {1, 1000000},
      0,
      3003000,
      std::nullopt,
      std::nullopt,
  };
  probe.video_streams.push_back(svp::media::VideoStreamProbe{
      "vstream_0001",
      0,
      "h264",
      1920,
      1080,
      0,
      {1, 1},
      {{1, 30000}, 0, 90090, {{30000, 1001}}, 90},
  });
  probe.audio_streams.push_back(svp::media::AudioStreamProbe{
      "astream_0001",
      1,
      "aac",
      48000,
      2,
      {{1, 48000}, 0, 144144, std::nullopt, std::nullopt},
  });

  const nlohmann::json encoded = svp::media::media_probe_to_json(probe);
  assert(encoded["video_streams"][0].contains("timing"));
  assert(!encoded["video_streams"][0].contains("timebase"));

  const svp::media::MediaProbe parsed =
      svp::media::parse_media_probe_json(encoded, "round-trip");
  assert(parsed.container_timing.has_value());
  assert(parsed.container_timing->duration_pts == 3003000);
  assert(parsed.video_streams.size() == 1);
  assert(parsed.video_streams[0].timing.timebase.numerator == 1);
  assert(parsed.video_streams[0].timing.timebase.denominator == 30000);
  assert(parsed.video_streams[0].timing.duration_pts == 90090);
  assert(parsed.video_streams[0].timing.average_frame_rate.has_value());
  assert(parsed.video_streams[0].timing.average_frame_rate->numerator == 30000);
  assert(parsed.video_streams[0].timing.average_frame_rate->denominator == 1001);
  assert(parsed.video_streams[0].timing.frame_count == 90);
  assert(parsed.audio_streams.size() == 1);
  assert(parsed.audio_streams[0].timing.timebase.denominator == 48000);
}

void test_probe_json_parser_keeps_legacy_flat_timing() {
  const nlohmann::json value = {
      {"format_name", "matroska,webm"},
      {"video_streams",
       {{{"id", "vstream_0001"},
         {"index", 0},
         {"codec_name", "vp9"},
         {"width", 1080},
         {"height", 1920},
         {"pixel_aspect_ratio", "1:1"},
         {"timebase", "1/1000"},
         {"start_pts", 12},
         {"duration_pts", 3456},
         {"avg_frame_rate", "30/1"},
         {"frame_count", 104}}}},
      {"audio_streams", nlohmann::json::array()},
  };

  const svp::media::MediaProbe parsed =
      svp::media::parse_media_probe_json(value, "legacy-flat");
  assert(parsed.video_streams[0].timing.timebase.denominator == 1000);
  assert(parsed.video_streams[0].timing.start_pts == 12);
  assert(parsed.video_streams[0].timing.duration_pts == 3456);
  assert(parsed.video_streams[0].timing.frame_count == 104);
}

void test_ffprobe_side_data_rotation_feeds_canonical_raster() {
  const nlohmann::json value = {
      {"format_name", "mov,mp4,m4a,3gp,3g2,mj2"},
      {"video_streams",
       {{{"id", "vstream_0001"},
         {"index", 0},
         {"codec_name", "h264"},
         {"width", 1920},
         {"height", 1080},
         {"pixel_aspect_ratio", "1:1"},
         {"timing", {{"timebase", "1/30000"}, {"start_pts", 0}}},
         {"side_data_list",
          {{{"side_data_type", "Display Matrix"}, {"rotation", 90}}}}}}},
      {"audio_streams", nlohmann::json::array()},
  };

  const svp::media::MediaProbe parsed =
      svp::media::parse_media_probe_json(value, "side-data-rotation");
  assert(parsed.video_streams[0].rotation_degrees == 90);

  const svp::media::MediaIngestPlan plan =
      svp::media::build_media_ingest_plan("rotated-phone.mov", parsed);
  assert(plan.canonical_raster.width == 360);
  assert(plan.canonical_raster.height == 640);
}

}  // namespace

int main() {
  test_round_half_to_even();
  test_frame_rates();
  test_stream_pts_are_normalized_against_primary_presentation_start();
  test_canonical_rasters();
  test_probe_json_round_trip_preserves_nested_timing();
  test_probe_json_parser_keeps_legacy_flat_timing();
  test_ffprobe_side_data_rotation_feeds_canonical_raster();
  return 0;
}
