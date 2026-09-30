#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace svp::package {

struct LogicalRowStreamSummary {
  std::string blake3;
  std::uint64_t table_count = 0;
  std::uint64_t row_count = 0;
};

// One table as it appears in the RC2 Section 17.5 canonical logical row
// stream: name, normalized schema fingerprint, column names in ordinal
// schema order, and declared primary-key columns in key order (empty when
// the table declares none).
struct LogicalTableDescriptor {
  std::string name;
  std::string schema_fingerprint;
  std::vector<std::string> column_names;
  std::vector<std::string> primary_key_columns;
};

// Receives the canonical logical row stream in stream order. Each row value
// is the SVP canonical JSON scalar used by the stream, for example
// ["null"], ["integer", 7], ["real", 0.5], ["text", "<utf8 hex>"],
// ["blob", "<hex>"]. Rows arrive in canonical row order (Section 17.5
// steps 7 and 8).
class LogicalRowStreamVisitor {
 public:
  virtual ~LogicalRowStreamVisitor() = default;
  virtual void on_table(const LogicalTableDescriptor& table) = 0;
  virtual void on_row(const LogicalTableDescriptor& table,
                      const nlohmann::json& values) = 0;
};

void visit_logical_row_stream(sqlite3& database,
                              const std::set<std::string>& table_names,
                              LogicalRowStreamVisitor& visitor);

[[nodiscard]] LogicalRowStreamSummary compute_logical_row_stream_summary(
    sqlite3& database,
    const std::set<std::string>& table_names);

}  // namespace svp::package
