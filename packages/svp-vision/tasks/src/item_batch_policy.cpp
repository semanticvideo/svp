#include "svp/vision/tasks/item_batch_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace svp::vision::tasks {

void validate_item_batch_policy(const ItemBatchPolicy& policy) {
  if (!std::isfinite(policy.target_task_seconds) || policy.target_task_seconds <= 0.0) {
    throw std::invalid_argument("ItemBatchPolicy.target_task_seconds must be finite and > 0");
  }
  if (!std::isfinite(policy.estimated_seconds_per_item) ||
      policy.estimated_seconds_per_item <= 0.0) {
    throw std::invalid_argument(
        "ItemBatchPolicy.estimated_seconds_per_item must be finite and > 0");
  }
  if (policy.max_parameter_bytes == 0 || policy.max_result_bytes == 0) {
    throw std::invalid_argument("ItemBatchPolicy byte budgets must be > 0");
  }
}

std::uint64_t item_batch_size_by_time(const ItemBatchPolicy& policy) {
  validate_item_batch_policy(policy);
  const double items = std::floor(policy.target_task_seconds / policy.estimated_seconds_per_item);
  // A ratio too large for uint64 still means "everything in one batch".
  constexpr auto kMaxCount = std::numeric_limits<std::uint64_t>::max();
  if (!(items < static_cast<double>(kMaxCount))) {
    return kMaxCount;
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(items));
}

namespace {

// Whether `used` plus `more` stays within `budget` (no overflow).
bool fits(std::uint64_t used, std::uint64_t more, std::uint64_t budget) {
  return used <= budget && more <= budget - used;
}

}  // namespace

std::vector<ItemBatch> partition_items(
    std::size_t item_count, const ItemBatchPolicy& policy,
    const std::function<ItemBytes(std::size_t index)>& item_bytes) {
  const std::uint64_t by_time = item_batch_size_by_time(policy);
  std::vector<ItemBatch> batches;
  ItemBatch current;
  ItemBytes used;
  for (std::size_t index = 0; index < item_count; ++index) {
    const ItemBytes bytes = item_bytes ? item_bytes(index) : ItemBytes{};
    const bool full = current.count == by_time ||
                      (current.count > 0 &&
                       (!fits(used.parameter_bytes, bytes.parameter_bytes,
                              policy.max_parameter_bytes) ||
                        !fits(used.result_bytes, bytes.result_bytes, policy.max_result_bytes)));
    if (full) {
      batches.push_back(current);
      current = ItemBatch{};
      used = ItemBytes{};
    }
    if (current.count == 0) {
      current.first = index;
    }
    ++current.count;
    used.parameter_bytes += bytes.parameter_bytes;
    used.result_bytes += bytes.result_bytes;
  }
  if (current.count > 0) {
    batches.push_back(current);
  }
  return batches;
}

std::uint64_t item_batch_estimated_seconds(const ItemBatchPolicy& policy, std::uint64_t count) {
  validate_item_batch_policy(policy);
  const double seconds = std::ceil(static_cast<double>(count) * policy.estimated_seconds_per_item);
  if (!(seconds < static_cast<double>(std::numeric_limits<std::uint64_t>::max()))) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(seconds));
}

}  // namespace svp::vision::tasks
