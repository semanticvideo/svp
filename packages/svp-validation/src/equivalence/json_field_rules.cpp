#include "equivalence/json_field_rules.hpp"

#include "equivalence/equivalence_profile.hpp"

#include <array>

namespace svp::validation::equivalence {
namespace {

using Profile = DefaultEquivalenceProfileV1;

constexpr std::string_view kLoudnessEntry = "media/audio/loudness.jsonl";
constexpr std::string_view kLoudnessSummaryEntry = "media/audio/loudness_summary.json";
constexpr std::string_view kSpectrumEntry = "media/audio/spectrum.jsonl";
constexpr std::string_view kSpectrumSummaryEntry = "media/audio/spectrum_summary.json";
constexpr std::string_view kColorObservationsEntry = "colors/color_observations.jsonl";
constexpr std::string_view kTextRegionsEntry = "text/text_regions.jsonl";
constexpr std::string_view kSpatialRegionsEntry = "spatial/regions.jsonl";

constexpr JsonFieldRule loudness(std::string_view entry, std::string_view key) {
  return {.rule_id = "loudness_observations",
          .entry = entry,
          .key = key,
          .tolerance = Profile::kLoudnessAbsMaxLu};
}

constexpr JsonFieldRule true_peak(std::string_view entry) {
  return {.rule_id = "loudness_true_peak",
          .entry = entry,
          .key = "true_peak_dbtp",
          .tolerance = Profile::kTruePeakAbsMaxDbtp};
}

constexpr JsonFieldRule spectrum(std::string_view entry, std::string_view key) {
  return {.rule_id = "spectrum_observations",
          .entry = entry,
          .key = key,
          .tolerance = Profile::kSpectrumAbsMaxDb};
}

constexpr JsonFieldRule box_iou(std::string_view rule_id,
                                std::string_view entry,
                                std::string_view key) {
  return {.rule_id = rule_id,
          .entry = entry,
          .key = key,
          .kind = JsonFieldRuleKind::bounding_box_iou,
          .tolerance = Profile::kBoundingBoxIouMin};
}

// Section 5.16.2 rows, one entry per governed field.
constexpr std::array kJsonFieldRules{
    // "Confidence fields" (also referenced by OCR text regions and text
    // observations). Parameters such as *_confidence_threshold do not end in
    // "_confidence" and stay exact.
    JsonFieldRule{.rule_id = "confidence_fields",
                  .key = "confidence",
                  .tolerance = Profile::kConfidenceAbsMax},
    JsonFieldRule{.rule_id = "confidence_fields",
                  .key = "_confidence",
                  .match = JsonKeyMatch::suffix,
                  .tolerance = Profile::kConfidenceAbsMax},

    // "Loudness observations".
    loudness(kLoudnessEntry, "momentary_lufs"),
    loudness(kLoudnessEntry, "shortterm_lufs"),
    true_peak(kLoudnessEntry),
    loudness(kLoudnessSummaryEntry, "integrated_lufs"),
    loudness(kLoudnessSummaryEntry, "loudness_range_lu"),
    loudness(kLoudnessSummaryEntry, "lra_low_lufs"),
    loudness(kLoudnessSummaryEntry, "lra_high_lufs"),
    loudness(kLoudnessSummaryEntry, "max_momentary_lufs"),
    loudness(kLoudnessSummaryEntry, "max_shortterm_lufs"),
    true_peak(kLoudnessSummaryEntry),

    // "Spectrum observations".
    spectrum(kSpectrumEntry, "bands"),
    spectrum(kSpectrumSummaryEntry, "mean_band_dbfs"),
    spectrum(kSpectrumSummaryEntry, "max_band_dbfs"),

    // "Color observations".
    JsonFieldRule{.rule_id = "color_bucket_coverage",
                  .entry = kColorObservationsEntry,
                  .key = "bucket_coverage",
                  .tolerance = Profile::kColorBucketCoverageAbsMax},
    JsonFieldRule{.rule_id = "color_coverage_total",
                  .entry = kColorObservationsEntry,
                  .key = "coverage_total",
                  .tolerance = Profile::kColorCoverageTotalAbsMax},
    JsonFieldRule{.rule_id = "color_dominant_bucket",
                  .entry = kColorObservationsEntry,
                  .key = "dominant_bucket",
                  .kind = JsonFieldRuleKind::dominant_bucket_tie,
                  .tolerance = Profile::kColorBucketCoverageAbsMax},

    // "OCR text regions".
    box_iou("ocr_text_region_bbox", kTextRegionsEntry, "bbox_norm"),
    box_iou("ocr_text_region_bbox", kTextRegionsEntry, "bbox_px"),

    // "Entity tracks": per-frame region boxes and centroids.
    box_iou("entity_track_bbox", kSpatialRegionsEntry, "box_norm"),
    box_iou("entity_track_bbox", kSpatialRegionsEntry, "box_px"),
    JsonFieldRule{.rule_id = "entity_track_centroid",
                  .entry = kSpatialRegionsEntry,
                  .key = "centroid_norm",
                  .kind = JsonFieldRuleKind::centroid_displacement_px,
                  .tolerance = Profile::kCentroidDisplacementPxMax},
};

constexpr std::array<std::string_view, 6> kExactFallbackNotes{
    "Human-readable *_sec strings are compared exactly; Section 5.16.2 would "
    "allow ignoring them for shot boundaries when *_us and frame indices match.",
    "Kalman-smoothed and optical-flow-derived coordinates: no RC2 package field "
    "declares this source, so such values are compared exactly.",
    "Floating-point scores without a Section 5.16.2 row (for example "
    "quality_score, legibility_score, contrast_ratio, *_score, "
    "screen_area_ratio, depth_summary, waveform peak_db/rms_db) are compared "
    "exactly.",
    "Confidence fields use the profile default; RC2 schemas define no field "
    "through which a processor record declares a tighter tolerance.",
    "Canonical JSON comparison validates UTF-8 but applies no Unicode "
    "normalization form; text that differs only by NFC/NFD form is not "
    "equivalent.",
    "SQLite REAL columns other than confidence and color coverage (for "
    "example spatial_regions.screen_area_ratio) are compared exactly.",
};

bool key_matches(const JsonFieldRule& rule, std::string_view key) {
  if (rule.match == JsonKeyMatch::exact) {
    return key == rule.key;
  }
  return key.size() > rule.key.size() && key.ends_with(rule.key);
}

}  // namespace

const JsonFieldRule* find_json_field_rule(std::string_view entry, std::string_view key) {
  for (const auto& rule : kJsonFieldRules) {
    if ((rule.entry.empty() || rule.entry == entry) && key_matches(rule, key)) {
      return &rule;
    }
  }
  return nullptr;
}

std::span<const std::string_view> exact_fallback_notes() {
  return kExactFallbackNotes;
}

}  // namespace svp::validation::equivalence
