#pragma once

#include <string_view>

namespace svp::validation::equivalence {

// Default Equivalence Profile v1: the numeric thresholds of SVP RC2
// Section 5.16.2, mirrored by spec/registries/equivalence-profile.json.
// This object is the single owner of every tolerance used by the package
// equivalence comparator. Nothing else in the comparator may introduce a
// tolerance literal; a unit test cross-checks these values against the
// registry so the code and the spec asset cannot drift apart.
struct DefaultEquivalenceProfileV1 {
  static constexpr std::string_view kName = "svp-default-equivalence-profile-v1";

  // Section 5.16.2 "Confidence fields": absolute difference <= 0.001 unless
  // the processor record declares a tighter tolerance. RC2 schemas define no
  // per-record tolerance field, so the profile default always applies.
  static constexpr double kConfidenceAbsMax = 0.001;

  // Section 5.16.2 "Loudness observations": LUFS/LU fields <= 0.1 LU and
  // true_peak_dbtp <= 0.1 dBTP. null matches only null.
  static constexpr double kLoudnessAbsMaxLu = 0.1;
  static constexpr double kTruePeakAbsMaxDbtp = 0.1;

  // Section 5.16.2 "Spectrum observations": bands, mean_band_dbfs, and
  // max_band_dbfs elements <= 0.1 dB. null matches only null.
  static constexpr double kSpectrumAbsMaxDb = 0.1;

  // Section 5.16.2 "Color observations": bucket coverage <= 0.005, coverage
  // totals within 0.001, dominant bucket exact unless tied within tolerance.
  static constexpr double kColorBucketCoverageAbsMax = 0.005;
  static constexpr double kColorCoverageTotalAbsMax = 0.001;

  // Section 5.16.2 "OCR text regions" and "Entity tracks": bounding boxes
  // IoU >= 0.995; entity centroid displacement <= 1.0 canonical analysis
  // raster pixel.
  static constexpr double kBoundingBoxIouMin = 0.995;
  static constexpr double kCentroidDisplacementPxMax = 1.0;

  // Section 5.16.2 "Depth maps": per block MAE <= 0.002, p99 absolute error
  // <= 0.010, Spearman rank correlation >= 0.995 on decoded normalized depth.
  static constexpr double kDepthMeanAbsErrorMax = 0.002;
  static constexpr double kDepthP99AbsErrorMax = 0.010;
  static constexpr double kDepthP99Quantile = 0.99;
  static constexpr double kDepthSpearmanMin = 0.995;

  // Section 5.16.2 "Embeddings": cosine similarity >= 0.999 between
  // corresponding normalized vectors. Section 17.5 applies the same rule to
  // vector_index.embedding.
  static constexpr double kEmbeddingCosineMin = 0.999;

  // Section 5.16.2 "Masks": non-empty masks IoU >= 0.995 and p95 boundary
  // displacement <= 1.0 canonical analysis raster pixel; empty masks match
  // only empty masks.
  static constexpr double kMaskIouMin = 0.995;
  static constexpr double kMaskBoundaryDisplacementQuantile = 0.95;
  static constexpr double kMaskBoundaryDisplacementPxMax = 1.0;
};

// Section 14.4: depth payloads are uint16 relative inverse depth where 0 is
// the farthest and 65535 the nearest valid depth in the frame. Decoded
// normalized depth is value / kDepthUint16FullScale.
inline constexpr double kDepthUint16FullScale = 65535.0;

}  // namespace svp::validation::equivalence
