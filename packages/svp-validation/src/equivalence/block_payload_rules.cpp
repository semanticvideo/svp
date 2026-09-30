#include "equivalence/block_payload_rules.hpp"

#include "equivalence/equivalence_profile.hpp"
#include "equivalence/mask_metrics.hpp"
#include "equivalence/payload_metrics.hpp"

#include <algorithm>
#include <utility>

namespace svp::validation::equivalence {
namespace {

using Profile = DefaultEquivalenceProfileV1;
using svp::blocks::BlockType;
using svp::blocks::DType;

void add_block_finding(const BlockPairContext& context,
                       EquivalenceLedger& ledger,
                       EquivalenceFinding finding,
                       MetricDirection direction = MetricDirection::lower_is_better) {
  finding.entry = context.entry;
  finding.location = context.location;
  ledger.mark_outcome(context.source_key, finding.outcome);
  ledger.add(std::move(finding), direction);
}

void add_measured(const BlockPairContext& context,
                  EquivalenceLedger& ledger,
                  std::string_view layer,
                  std::string_view rule,
                  double measured,
                  double tolerance,
                  MetricDirection direction) {
  const bool passes = direction == MetricDirection::lower_is_better ? measured <= tolerance
                                                                    : measured >= tolerance;
  add_block_finding(context, ledger,
                    EquivalenceFinding{
                        .outcome = passes ? EquivalenceOutcome::within_tolerance
                                          : EquivalenceOutcome::not_equivalent,
                        .layer = std::string{layer},
                        .rule = std::string{rule},
                        .detail = passes ? "within Default Equivalence Profile v1 tolerance"
                                         : "exceeds Default Equivalence Profile v1 tolerance",
                        .measured = measured,
                        .tolerance = tolerance,
                    },
                    direction);
}

void add_exact_fallback(const BlockPairContext& context,
                        EquivalenceLedger& ledger,
                        std::string_view layer,
                        std::string detail) {
  add_block_finding(context, ledger,
                    EquivalenceFinding{
                        .outcome = EquivalenceOutcome::not_equivalent,
                        .layer = std::string{layer},
                        .rule = "exact_fallback",
                        .detail = std::move(detail),
                    });
}

void compare_depth(const BlockPairContext& context,
                   const svp::blocks::BlockHeaderV1& header,
                   std::span<const std::byte> left,
                   std::span<const std::byte> right,
                   EquivalenceLedger& ledger) {
  constexpr std::string_view kLayer = "depth_block";
  if (header.dtype != static_cast<std::uint32_t>(DType::uint16)) {
    add_exact_fallback(context, ledger, kLayer,
                       "decoded payload differs; depth dtype " +
                           std::to_string(header.dtype) +
                           " has no normalized-depth decoder, compared exactly");
    return;
  }
  const auto metrics = depth_metrics(decode_uint16_le(left), decode_uint16_le(right));
  add_measured(context, ledger, kLayer, "depth_maps.mean_absolute_error",
               metrics.mean_absolute_error, Profile::kDepthMeanAbsErrorMax,
               MetricDirection::lower_is_better);
  add_measured(context, ledger, kLayer, "depth_maps.p99_absolute_error",
               metrics.p99_absolute_error, Profile::kDepthP99AbsErrorMax,
               MetricDirection::lower_is_better);
  add_measured(context, ledger, kLayer, "depth_maps.spearman_rank_correlation",
               metrics.spearman, Profile::kDepthSpearmanMin,
               MetricDirection::higher_is_better);
}

void compare_embeddings(const BlockPairContext& context,
                        const svp::blocks::BlockHeaderV1& header,
                        std::span<const std::byte> left,
                        std::span<const std::byte> right,
                        EquivalenceLedger& ledger) {
  constexpr std::string_view kLayer = "embedding_block";
  if (header.dtype != static_cast<std::uint32_t>(DType::float32) || header.extent_1 == 0) {
    add_exact_fallback(context, ledger, kLayer,
                       "decoded payload differs; embedding block is not float32 "
                       "vectors, compared exactly");
    return;
  }
  const auto left_values = decode_float32_le(left);
  const auto right_values = decode_float32_le(right);
  const std::size_t dimension = header.extent_1;
  double worst = 1.0;
  for (std::size_t start = 0; start + dimension <= left_values.size(); start += dimension) {
    worst = std::min(worst, cosine_similarity(
                                std::span{left_values}.subspan(start, dimension),
                                std::span{right_values}.subspan(start, dimension)));
  }
  add_measured(context, ledger, kLayer, "embeddings.cosine_similarity", worst,
               Profile::kEmbeddingCosineMin, MetricDirection::higher_is_better);
}

void compare_masks(const BlockPairContext& context,
                   const svp::blocks::BlockHeaderV1& header,
                   std::span<const std::byte> left,
                   std::span<const std::byte> right,
                   EquivalenceLedger& ledger) {
  constexpr std::string_view kLayer = "mask_block";
  const auto left_planes =
      decode_mask_planes(left, header.dtype, header.extent_0, header.extent_1, header.extent_2);
  const auto right_planes =
      decode_mask_planes(right, header.dtype, header.extent_0, header.extent_1, header.extent_2);
  if (!left_planes || !right_planes) {
    add_exact_fallback(context, ledger, kLayer,
                       "decoded payload differs; mask payload could not be decoded "
                       "to width*height*planes pixels, compared exactly");
    return;
  }
  for (std::size_t plane = 0; plane < left_planes->size(); ++plane) {
    const auto& left_plane = (*left_planes)[plane];
    const auto& right_plane = (*right_planes)[plane];
    if (left_plane.pixels == right_plane.pixels) {
      continue;
    }
    if (left_plane.empty() || right_plane.empty()) {
      add_block_finding(context, ledger,
                        EquivalenceFinding{
                            .outcome = EquivalenceOutcome::not_equivalent,
                            .layer = std::string{kLayer},
                            .rule = "masks.empty_matches_only_empty",
                            .detail = "plane " + std::to_string(plane) +
                                      ": an empty mask is equivalent only to an empty mask",
                        });
      continue;
    }
    const auto metrics = mask_metrics(left_plane, right_plane);
    add_measured(context, ledger, kLayer, "masks.iou", metrics.iou, Profile::kMaskIouMin,
                 MetricDirection::higher_is_better);
    add_measured(context, ledger, kLayer, "masks.p95_boundary_displacement_px",
                 metrics.boundary_displacement_px, Profile::kMaskBoundaryDisplacementPxMax,
                 MetricDirection::lower_is_better);
  }
}

}  // namespace

void compare_decoded_block_payloads(const BlockPairContext& context,
                                    const svp::blocks::BlockHeaderV1& header,
                                    std::span<const std::byte> left,
                                    std::span<const std::byte> right,
                                    EquivalenceLedger& ledger) {
  switch (static_cast<BlockType>(header.block_type)) {
    case BlockType::depth:
      compare_depth(context, header, left, right, ledger);
      return;
    case BlockType::embedding:
      compare_embeddings(context, header, left, right, ledger);
      return;
    case BlockType::mask:
      compare_masks(context, header, left, right, ledger);
      return;
  }
  add_exact_fallback(context, ledger, "block",
                     "decoded payload differs; unknown block type, compared exactly");
}

}  // namespace svp::validation::equivalence
