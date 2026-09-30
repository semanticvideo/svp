// The frame plan must reproduce the frame IDs that stage-order registration
// produced before frames were planned up front. The legacy model below is an
// independent restatement of the pre-plan stage sequence (color decode,
// canonical decode, OCR samples, tracking windows), each registering as its
// decoder did: in request order, first decoded frame flagged as keyframe.

#include "svp/media/media_ingest_plan.hpp"
#include "svp/vision/canonical_frame_input.hpp"
#include "svp/vision/frame_catalog.hpp"
#include "svp/vision/frame_plan.hpp"
#include "svp/vision/ocr_generation.hpp"
#include "svp/vision/ocr_temporal_sampling.hpp"
#include "svp/vision/real_frame_color_sampling.hpp"
#include "svp/vision/visual_entity_sampling.hpp"
#include "svp/vision/visual_tracking_quality.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using svp::vision::FrameCatalog;
using svp::vision::FramePlanInputs;
using svp::vision::VisualTrackingQuality;

int g_failures = 0;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++g_failures;
  }
}

constexpr VisualTrackingQuality kAllQualities[] = {
    VisualTrackingQuality::off, VisualTrackingQuality::low,
    VisualTrackingQuality::medium, VisualTrackingQuality::high};

// Durations chosen to exercise distinct schedule shapes: unknown duration,
// shorter than the OCR minimum-sample path, a partial last tracking window,
// a window end landing exactly on the duration, and several windows.
constexpr std::int64_t kDurationsUs[] = {
    0, 400'000, 2'500'000, 20'000'000, 30'150'000, 125'432'100};

struct StageSelection {
  bool color;
  bool canonical_and_ocr;
  std::string name;
};

// --stop-after foundation-color, foundation-ocr, package (and the empty
// selections media-ingest/audio/vision-plan).
const std::vector<StageSelection> kStageSelections = {
    {false, false, "no frame stages"},
    {true, false, "foundation-color"},
    {false, true, "foundation-ocr"},
    {true, true, "package"},
};

// Pre-plan registration sequence: stages call register_frame on an open
// catalog after each successful decode. decoded(ts, purpose) says whether a
// request decodes; a miss skips registration and moves the keyframe flag to the
// stage's next decoded frame, as the decoders did.
template <typename Decoded>
FrameCatalog legacy_catalog(const FramePlanInputs& in, Decoded decoded) {
  FrameCatalog catalog;
  const bool raster = in.canonical_width > 0 && in.canonical_height > 0;
  auto seek_stage = [&](int count, const char* purpose) {
    bool first = true;
    for (auto ts : svp::vision::deterministic_seek_timestamps_us(
             in.duration_us, count)) {
      if (!decoded(ts, purpose)) continue;
      catalog.register_frame(ts, purpose, first);
      first = false;
    }
  };
  if (in.color && raster && in.duration_us > 0)
    seek_stage(svp::vision::kColorDecodedFrameCount, "color");
  if (in.canonical && raster && in.duration_us > 0)
    seek_stage(svp::vision::kCanonicalDecodedFrameCount, "canonical");
  if (in.ocr && in.ocr_frame_width > 0 && in.ocr_frame_height > 0) {
    auto sampling = svp::vision::compute_ocr_temporal_timestamps(
        in.duration_us, in.ocr_sampling);
    if (in.ocr_diagnostic_override)
      sampling.timestamps_us = *in.ocr_diagnostic_override;
    bool first = true;
    for (auto ts : sampling.timestamps_us) {
      if (!decoded(ts, "ocr")) continue;
      catalog.register_frame(ts, "ocr", first);
      first = false;
    }
  }
  if (in.visual_tracking != VisualTrackingQuality::off && raster) {
    const auto policy =
        svp::vision::visual_tracking_quality_policy(in.visual_tracking);
    const svp::vision::VisualEntitySamplingOptions sampling{
        policy.sample_interval_us, policy.window_duration_us,
        policy.window_overlap_us};
    for (const auto& window : svp::vision::make_visual_entity_sampling_plan(
             in.duration_us, sampling)) {
      // The window decoder reads one ffmpeg stream and stops at the first
      // short read, so a miss drops the rest of the window.
      for (std::size_t i = 0; i < window.timestamps_us.size(); ++i) {
        if (!decoded(window.timestamps_us[i], "visual_entity_tracking"))
          break;
        catalog.register_frame(window.timestamps_us[i],
                               "visual_entity_tracking", i == 0);
      }
    }
  }
  return catalog;
}

FrameCatalog legacy_catalog(const FramePlanInputs& in) {
  return legacy_catalog(in, [](std::int64_t, const char*) { return true; });
}

FrameCatalog planned_catalog(const FramePlanInputs& in) {
  FrameCatalog catalog;
  svp::vision::load_frame_plan(catalog,
                               svp::vision::plan_frame_registrations(in));
  return catalog;
}

// Replays the stage registrations against a locked catalog, as the stages do
// after the plan is loaded.
template <typename Decoded>
void replay_stages(FrameCatalog& locked, const FramePlanInputs& in,
                   Decoded decoded) {
  const FrameCatalog legacy = legacy_catalog(in, decoded);
  for (const auto& entry : legacy.entries()) {
    for (const auto& purpose : entry.purposes)
      locked.register_frame(entry.timestamp_us, purpose, entry.keyframe);
  }
}

bool same_entries(const std::vector<svp::vision::FrameCatalogEntry>& a,
                  const std::vector<svp::vision::FrameCatalogEntry>& b,
                  bool compare_flags) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].frame_id != b[i].frame_id ||
        a[i].frame_index != b[i].frame_index ||
        a[i].timestamp_us != b[i].timestamp_us)
      return false;
    if (compare_flags &&
        (a[i].keyframe != b[i].keyframe || a[i].purposes != b[i].purposes))
      return false;
  }
  return true;
}

FramePlanInputs inputs_for(std::int64_t duration_us,
                           const StageSelection& stages,
                           VisualTrackingQuality quality) {
  FramePlanInputs in;
  in.duration_us = duration_us;
  in.canonical_width = 640;
  in.canonical_height = 360;
  in.ocr_frame_width = 1920;
  in.ocr_frame_height = 1080;
  in.color = stages.color;
  in.canonical = stages.canonical_and_ocr;
  in.ocr = stages.canonical_and_ocr;
  in.visual_tracking =
      stages.canonical_and_ocr ? quality : VisualTrackingQuality::off;
  return in;
}

std::string describe(std::int64_t duration_us, const StageSelection& stages,
                     VisualTrackingQuality quality) {
  return "duration=" + std::to_string(duration_us) + "us stages=" +
         stages.name + " quality=" +
         std::string(svp::vision::visual_tracking_quality_name(quality));
}

void test_plan_reproduces_legacy_registration_order() {
  for (const auto duration_us : kDurationsUs) {
    for (const auto& stages : kStageSelections) {
      for (const auto quality : kAllQualities) {
        const FramePlanInputs in = inputs_for(duration_us, stages, quality);
        const FrameCatalog legacy = legacy_catalog(in);
        FrameCatalog planned = planned_catalog(in);
        const std::string where = describe(duration_us, stages, quality);

        require(same_entries(planned.planned_entries(), legacy.entries(),
                             true),
                "planned IDs, timestamps, purposes, keyframes match legacy: " +
                    where);
        require(planned.entries().empty(),
                "no planned frame counts as decoded before stages run: " +
                    where);

        replay_stages(planned, in,
                      [](std::int64_t, const char*) { return true; });
        require(same_entries(planned.entries(), legacy.entries(), true),
                "decoded entries after stages match legacy: " + where);
        require(planned.size() == legacy.size(),
                "size matches legacy: " + where);
      }
    }
  }
}

void test_known_cross_stage_ids() {
  // 30.15 s package: every canonical seek folds onto a color seek (the
  // canonical slice midpoints are a subset of the color midpoints), and OCR
  // samples follow all color frames. This is the measured excerpt where the
  // t=0 OCR frame became frame_000016.
  const FramePlanInputs in = inputs_for(
      30'150'000, kStageSelections[3], VisualTrackingQuality::off);
  const FrameCatalog planned = planned_catalog(in);
  const auto color_count =
      static_cast<std::size_t>(svp::vision::kColorDecodedFrameCount);
  const auto ocr_count = svp::vision::compute_ocr_temporal_timestamps(
                             in.duration_us, in.ocr_sampling)
                             .timestamps_us.size();
  require(planned.planned_entries().size() == color_count + ocr_count,
          "color + OCR frames with canonical folded onto color");
  require(planned.get_frame_index(std::int64_t{0}) == color_count &&
              planned.planned_entries()[color_count].frame_id ==
                  "frame_000016",
          "the first OCR sample follows all color frames");
  for (const auto ts : svp::vision::deterministic_seek_timestamps_us(
           in.duration_us, svp::vision::kCanonicalDecodedFrameCount)) {
    const auto index = planned.get_frame_index(ts);
    require(index.has_value() && *index < color_count,
            "canonical seeks reuse color frames");
  }

  // foundation-ocr decodes no color frames, so canonical frames lead.
  const FrameCatalog ocr_only = planned_catalog(inputs_for(
      30'150'000, kStageSelections[2], VisualTrackingQuality::off));
  require(ocr_only.get_frame_index(std::int64_t{0}) ==
              static_cast<std::size_t>(
                  svp::vision::kCanonicalDecodedFrameCount),
          "foundation-ocr places the first OCR sample after the canonical "
          "frames");
}

void test_ocr_diagnostic_override() {
  const std::vector<std::int64_t> override_us = {0, 7'777'777, 12'345'678};
  for (const auto duration_us : kDurationsUs) {
    for (const auto quality : kAllQualities) {
      FramePlanInputs in =
          inputs_for(duration_us, kStageSelections[3], quality);
      in.ocr_diagnostic_override = override_us;
      const FrameCatalog legacy = legacy_catalog(in);
      const FrameCatalog planned = planned_catalog(in);
      require(same_entries(planned.planned_entries(), legacy.entries(), true),
              "override plan matches legacy: " +
                  describe(duration_us, kStageSelections[3], quality));
      for (const auto ts : override_us) {
        require(planned.get_frame_index(ts).has_value(),
                "override timestamp is planned even for unknown duration");
      }
    }
  }

  require(svp::vision::parse_ocr_diagnostic_timestamps("3,1,2,2") ==
              std::vector<std::int64_t>({1, 2, 3}),
          "override list is sorted and unique");
  require(!svp::vision::parse_ocr_diagnostic_timestamps("1,-2").has_value(),
          "negative override is rejected");
  require(!svp::vision::parse_ocr_diagnostic_timestamps("1;2").has_value(),
          "malformed override is rejected");
  require(!svp::vision::parse_ocr_diagnostic_timestamps("").has_value(),
          "empty override is ignored");
}

svp::media::MediaIngestPlan media_plan(std::int32_t width, std::int32_t height,
                                       std::int32_t rotation_degrees,
                                       std::int64_t duration_us) {
  svp::media::MediaIngestPlan plan;
  plan.primary_video_stream.width = width;
  plan.primary_video_stream.height = height;
  plan.primary_video_stream.rotation_degrees = rotation_degrees;
  plan.primary_video_stream.timing.timebase = {1, 1'000'000};
  plan.primary_video_stream.timing.duration_pts = duration_us;
  plan.canonical_raster = svp::media::compute_canonical_analysis_raster(
      {width, height, rotation_degrees, {1, 1}});
  return plan;
}

void test_media_inputs_and_rotations() {
  struct Case {
    std::int32_t width, height, rotation;
    int ocr_width, ocr_height;
  };
  // OCR decodes at source display size, swapped for +/-90 degrees and capped
  // at the OCR decode bound.
  const Case cases[] = {
      {1920, 1080, 0, 1920, 1080},
      {1920, 1080, 90, 1080, 1920},
      {1920, 1080, -90, 1080, 1920},
      {1920, 1080, 180, 1920, 1080},
      {3840, 2160, 0, 1920, 1080},
      {3840, 2160, 90, 1080, 1920},
      {1280, 720, 0, 1280, 720},
  };
  for (const auto& c : cases) {
    const auto plan = media_plan(c.width, c.height, c.rotation, 30'150'000);
    const auto in = svp::vision::frame_plan_inputs_for_media(plan);
    const std::string where = std::to_string(c.width) + "x" +
                              std::to_string(c.height) + "@" +
                              std::to_string(c.rotation);
    require(in.duration_us == 30'150'000, "duration from media: " + where);
    require(in.ocr_frame_width == c.ocr_width &&
                in.ocr_frame_height == c.ocr_height,
            "OCR decode dimensions: " + where);
    require(in.canonical_width == plan.canonical_raster.width &&
                in.canonical_height == plan.canonical_raster.height,
            "canonical raster from media: " + where);

    for (const auto quality : kAllQualities) {
      FramePlanInputs staged = in;
      staged.color = staged.canonical = staged.ocr = true;
      staged.visual_tracking = quality;
      require(same_entries(planned_catalog(staged).planned_entries(),
                           legacy_catalog(staged).entries(), true),
              "rotated media plan matches legacy: " + where);
    }
  }

  // No video stream: nothing decodes, so nothing is planned.
  FramePlanInputs empty =
      svp::vision::frame_plan_inputs_for_media(svp::media::MediaIngestPlan{});
  empty.color = empty.canonical = empty.ocr = true;
  empty.visual_tracking = VisualTrackingQuality::medium;
  require(svp::vision::plan_frame_registrations(empty).empty(),
          "no raster and no duration plans no frames");
}

void test_lookup_mode_rejects_unplanned_frames() {
  FrameCatalog catalog = planned_catalog(
      inputs_for(30'150'000, kStageSelections[3], VisualTrackingQuality::off));
  require(catalog.locked_to_plan(), "loaded plan locks the catalog");
  const auto planned_count = catalog.planned_entries().size();

  bool threw = false;
  try {
    catalog.register_frame(1, "ocr");
  } catch (const svp::vision::UnplannedFrameError&) {
    threw = true;
  }
  require(threw, "an unplanned timestamp throws in lookup mode");
  require(catalog.planned_entries().size() == planned_count,
          "a rejected lookup adds nothing");

  require(catalog.register_frame(0, "ocr", true) == "frame_000016",
          "a planned timestamp resolves to its planned ID");
  require(catalog.entries().size() == 1 &&
              catalog.entries().front().frame_id == "frame_000016",
          "only decoded frames are reported");

  bool reload_threw = false;
  try {
    svp::vision::load_frame_plan(catalog, {});
  } catch (const std::logic_error&) {
    reload_threw = true;
  }
  require(reload_threw, "a plan cannot be loaded twice");

  FrameCatalog open;
  require(!open.locked_to_plan(), "catalogs start in open mode");
  require(open.register_frame(42, "color") == "frame_000001" &&
              open.register_frame(7, "ocr") == "frame_000002" &&
              open.register_frame(42, "ocr") == "frame_000001",
          "open mode keeps first-registration IDs");
  require(open.entries().size() == 2 && open.size() == 2,
          "open mode reports every registered frame");
}

void test_trailing_window_miss_matches_legacy() {
  // The last tracking window can decode fewer frames than requested when
  // ffmpeg runs out of source at the end of the file (the window decoder
  // stops at the first short read). Only the final window reaches the end,
  // and its unshared timestamps are the last plan entries, so omitting them
  // keeps every other ID identical to legacy registration.
  for (const auto duration_us : kDurationsUs) {
    for (const auto quality : kAllQualities) {
      if (quality == VisualTrackingQuality::off || duration_us <= 0) continue;
      const FramePlanInputs in =
          inputs_for(duration_us, kStageSelections[3], quality);
      const auto policy = svp::vision::visual_tracking_quality_policy(quality);
      const auto windows = svp::vision::make_visual_entity_sampling_plan(
          duration_us, svp::vision::visual_entity_sampling_options(policy));
      const std::int64_t previous_window_end =
          windows.size() > 1 ? windows[windows.size() - 2].end_us : -1;
      // The final window loses everything in its last sample interval.
      const auto decoded = [&](std::int64_t ts, const char* purpose) {
        return std::string(purpose) != "visual_entity_tracking" ||
               ts <= previous_window_end ||
               ts < duration_us - policy.sample_interval_us;
      };
      FrameCatalog planned = planned_catalog(in);
      replay_stages(planned, in, decoded);
      const FrameCatalog legacy = legacy_catalog(in, decoded);
      require(same_entries(planned.entries(), legacy.entries(), false),
              "trailing window miss keeps legacy IDs: " +
                  describe(duration_us, kStageSelections[3], quality));
    }
  }
}

void test_mid_plan_miss_keeps_planned_ids() {
  // A miss before the end of the plan no longer shifts later IDs: the frame
  // keeps its planned ID and is simply not reported.
  const FramePlanInputs in = inputs_for(
      30'150'000, kStageSelections[3], VisualTrackingQuality::off);
  const auto color = svp::vision::deterministic_seek_timestamps_us(
      in.duration_us, 15);
  const std::int64_t missed = color[3];
  FrameCatalog planned = planned_catalog(in);
  replay_stages(planned, in, [&](std::int64_t ts, const char*) {
    return ts != missed;
  });
  const auto entries = planned.entries();
  require(entries.size() == planned.planned_entries().size() - 1,
          "the missed frame is not reported");
  require(planned.get_frame_index(std::int64_t{0}) == std::size_t{15},
          "later frames keep their planned index");
}

}  // namespace

int main() {
  test_plan_reproduces_legacy_registration_order();
  test_known_cross_stage_ids();
  test_ocr_diagnostic_override();
  test_media_inputs_and_rotations();
  test_lookup_mode_rejects_unplanned_frames();
  test_trailing_window_miss_matches_legacy();
  test_mid_plan_miss_keeps_planned_ids();
  if (g_failures != 0) {
    std::cerr << g_failures << " frame plan check(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "frame plan tests passed\n";
  return EXIT_SUCCESS;
}
