#pragma once

#include <cstdint>
#include <set>
#include <string>

struct sqlite3;

namespace svp::package {

struct LogicalRowStreamSummary {
  std::string blake3;
  std::uint64_t table_count = 0;
  std::uint64_t row_count = 0;
};

[[nodiscard]] LogicalRowStreamSummary compute_logical_row_stream_summary(
    sqlite3& database,
    const std::set<std::string>& table_names);

}  // namespace svp::package
