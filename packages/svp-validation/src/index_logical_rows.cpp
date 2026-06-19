#include "index_logical_rows.hpp"

#include <blake3.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace svp::validation {
namespace {

constexpr std::string_view kBlake3PatternPrefix = "blake3:";

struct TableColumn {
  std::string name;
  int primary_key_order = 0;
};

struct StatementDeleter {
  void operator()(sqlite3_stmt* statement) const noexcept {
    if (statement != nullptr) {
      sqlite3_finalize(statement);
    }
  }
};

std::string lower_hex(const std::uint8_t* bytes, std::size_t size) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return output.str();
}

std::string blake3_for_bytes(std::string_view bytes) {
  std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return std::string{kBlake3PatternPrefix} + lower_hex(digest.data(), digest.size());
}

std::string quoted_identifier(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  for (const auto character : value) {
    if (character == '"') {
      output.push_back('"');
    }
    output.push_back(character);
  }
  output.push_back('"');
  return output;
}

std::string normalized_sql(std::string value) {
  std::string output;
  output.reserve(value.size());
  bool pending_space = false;

  for (const auto character : value) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      pending_space = !output.empty();
      continue;
    }

    if (pending_space) {
      output.push_back(' ');
      pending_space = false;
    }
    output.push_back(character);
  }

  if (!output.empty() && output.back() == ' ') {
    output.pop_back();
  }
  return output;
}

nlohmann::json sqlite_value_to_json(sqlite3_stmt& statement, int column_index) {
  switch (sqlite3_column_type(&statement, column_index)) {
    case SQLITE_NULL:
      return nullptr;
    case SQLITE_INTEGER:
      return sqlite3_column_int64(&statement, column_index);
    case SQLITE_FLOAT:
      return sqlite3_column_double(&statement, column_index);
    case SQLITE_TEXT: {
      const auto* text = sqlite3_column_text(&statement, column_index);
      return text == nullptr ? nlohmann::json{std::string{}}
                             : nlohmann::json{
                                   std::string{reinterpret_cast<const char*>(text)}};
    }
    case SQLITE_BLOB: {
      const auto* blob = static_cast<const std::uint8_t*>(
          sqlite3_column_blob(&statement, column_index));
      const auto byte_count = sqlite3_column_bytes(&statement, column_index);
      if (blob == nullptr || byte_count <= 0) {
        return "blob:";
      }
      return "blob:" + lower_hex(blob, static_cast<std::size_t>(byte_count));
    }
    default:
      return nullptr;
  }
}

std::string json_line(const nlohmann::json& value) {
  return value.dump() + "\n";
}

void update_stream(blake3_hasher& hasher, const nlohmann::json& value) {
  const auto line = json_line(value);
  blake3_hasher_update(&hasher, line.data(), line.size());
}

std::vector<TableColumn> read_table_columns(sqlite3& database, std::string_view table_name) {
  const auto query = "PRAGMA table_info(" + quoted_identifier(table_name) + ")";
  sqlite3_stmt* raw_statement = nullptr;
  if (sqlite3_prepare_v2(&database, query.c_str(), -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }

  std::unique_ptr<sqlite3_stmt, StatementDeleter> statement{raw_statement};
  std::vector<TableColumn> columns;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }

    const auto* name_text = sqlite3_column_text(statement.get(), 1);
    if (name_text == nullptr) {
      continue;
    }

    columns.push_back(TableColumn{
        .name = reinterpret_cast<const char*>(name_text),
        .primary_key_order = sqlite3_column_int(statement.get(), 5),
    });
  }

  return columns;
}

std::string read_table_sql(sqlite3& database, std::string_view table_name) {
  constexpr std::string_view query =
      "SELECT sql FROM sqlite_schema WHERE name = ? AND type = 'table'";

  sqlite3_stmt* raw_statement = nullptr;
  if (sqlite3_prepare_v2(&database, query.data(), static_cast<int>(query.size()),
                         &raw_statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }

  std::unique_ptr<sqlite3_stmt, StatementDeleter> statement{raw_statement};
  sqlite3_bind_text(statement.get(), 1, table_name.data(),
                    static_cast<int>(table_name.size()), SQLITE_TRANSIENT);

  const auto step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) {
    return {};
  }
  if (step != SQLITE_ROW) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }

  const auto* sql_text = sqlite3_column_text(statement.get(), 0);
  return sql_text == nullptr ? std::string{} : reinterpret_cast<const char*>(sql_text);
}

std::string schema_fingerprint_for(sqlite3& database, std::string_view table_name) {
  nlohmann::json fingerprint_source = nlohmann::json::object();
  fingerprint_source["sql"] = normalized_sql(read_table_sql(database, table_name));
  fingerprint_source["table"] = std::string{table_name};
  fingerprint_source["columns"] = nlohmann::json::array();

  const auto query = "PRAGMA table_xinfo(" + quoted_identifier(table_name) + ")";
  sqlite3_stmt* raw_statement = nullptr;
  if (sqlite3_prepare_v2(&database, query.c_str(), -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }

  std::unique_ptr<sqlite3_stmt, StatementDeleter> statement{raw_statement};
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }

    nlohmann::json column = nlohmann::json::object();
    column["cid"] = sqlite3_column_int(statement.get(), 0);
    const auto* name_text = sqlite3_column_text(statement.get(), 1);
    const auto* type_text = sqlite3_column_text(statement.get(), 2);
    const auto* default_text = sqlite3_column_text(statement.get(), 4);
    column["name"] =
        name_text == nullptr ? "" : reinterpret_cast<const char*>(name_text);
    column["type"] =
        type_text == nullptr ? "" : reinterpret_cast<const char*>(type_text);
    column["notnull"] = sqlite3_column_int(statement.get(), 3);
    if (default_text == nullptr) {
      column["default"] = nullptr;
    } else {
      column["default"] = std::string{reinterpret_cast<const char*>(default_text)};
    }
    column["pk"] = sqlite3_column_int(statement.get(), 5);
    column["hidden"] = sqlite3_column_int(statement.get(), 6);
    fingerprint_source["columns"].push_back(std::move(column));
  }

  return blake3_for_bytes(fingerprint_source.dump());
}

std::string canonical_order_expression(std::string_view column_name) {
  const auto column = quoted_identifier(column_name);
  return "CASE typeof(" + column +
         ") WHEN 'null' THEN 0 WHEN 'integer' THEN 1 WHEN 'real' THEN 2 "
         "WHEN 'text' THEN 3 WHEN 'blob' THEN 4 ELSE 5 END, "
         "CASE WHEN typeof(" +
         column + ") IN ('integer', 'real') THEN " + column + " END, "
         "CASE WHEN typeof(" +
         column + ") = 'text' THEN " + column + " END COLLATE BINARY, "
         "CASE WHEN typeof(" +
         column + ") = 'blob' THEN hex(" + column + ") END";
}

std::string select_rows_query(std::string_view table_name,
                              const std::vector<TableColumn>& columns) {
  std::ostringstream query;
  query << "SELECT ";
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (index > 0) {
      query << ", ";
    }
    query << quoted_identifier(columns[index].name);
  }
  query << " FROM " << quoted_identifier(table_name);

  std::vector<TableColumn> primary_key_columns;
  for (const auto& column : columns) {
    if (column.primary_key_order > 0) {
      primary_key_columns.push_back(column);
    }
  }

  std::sort(primary_key_columns.begin(), primary_key_columns.end(),
            [](const TableColumn& left, const TableColumn& right) {
              return left.primary_key_order < right.primary_key_order;
            });

  const auto& order_columns =
      primary_key_columns.empty() ? columns : primary_key_columns;
  if (!order_columns.empty()) {
    query << " ORDER BY ";
    for (std::size_t index = 0; index < order_columns.size(); ++index) {
      if (index > 0) {
        query << ", ";
      }
      query << canonical_order_expression(order_columns[index].name);
    }
  }

  return query.str();
}

std::uint64_t emit_table_rows(sqlite3& database,
                              blake3_hasher& hasher,
                              std::string_view table_name,
                              const std::vector<TableColumn>& columns) {
  if (columns.empty()) {
    return 0;
  }

  const auto query = select_rows_query(table_name, columns);
  sqlite3_stmt* raw_statement = nullptr;
  if (sqlite3_prepare_v2(&database, query.c_str(), -1, &raw_statement, nullptr) !=
      SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(&database));
  }

  std::unique_ptr<sqlite3_stmt, StatementDeleter> statement{raw_statement};
  std::uint64_t row_count = 0;
  while (true) {
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      throw std::runtime_error(sqlite3_errmsg(&database));
    }

    nlohmann::json values = nlohmann::json::array();
    for (int index = 0; index < sqlite3_column_count(statement.get()); ++index) {
      values.push_back(sqlite_value_to_json(*statement, index));
    }
    update_stream(hasher,
                  nlohmann::json::array({"row", std::string{table_name}, values}));
    ++row_count;
  }

  return row_count;
}

}  // namespace

LogicalRowStreamSummary compute_logical_row_stream_summary(
    sqlite3& database,
    const std::set<std::string>& table_names) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  LogicalRowStreamSummary summary;
  summary.table_count = table_names.size();

  for (const auto& table_name : table_names) {
    const auto columns = read_table_columns(database, table_name);
    nlohmann::json column_names = nlohmann::json::array();
    for (const auto& column : columns) {
      column_names.push_back(column.name);
    }

    update_stream(hasher, nlohmann::json::array({"table", std::string{table_name}}));
    update_stream(
        hasher,
        nlohmann::json::array({"schema", std::string{table_name},
                               schema_fingerprint_for(database, table_name)}));
    update_stream(
        hasher,
        nlohmann::json::array({"columns", std::string{table_name}, column_names}));
    summary.row_count += emit_table_rows(database, hasher, table_name, columns);
  }

  std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  summary.blake3 =
      std::string{kBlake3PatternPrefix} + lower_hex(digest.data(), digest.size());
  return summary;
}

}  // namespace svp::validation
