#include "svp/query/query_reader.hpp"

#include "svp/package/package_layout.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace svp::query {
namespace {

bool is_blank_line(std::string_view line) {
  return std::ranges::all_of(line, [](unsigned char ch) { return std::isspace(ch) != 0; });
}

}  // namespace

JsonlReadResult read_jsonl_entry(const std::filesystem::path& package_path,
                                 const std::string& entry) {
  JsonlReadResult result;

  const auto layout = svp::package::read_package_layout(package_path);
  if (!layout.has_value()) {
    result.error_message = layout.error_message();
    return result;
  }

  result.present = layout.value().has_entry(entry);
  if (!result.present) {
    return result;
  }

  const auto read_result = svp::package::read_package_entry(package_path, entry);
  if (!read_result.has_value()) {
    result.error_message = read_result.error_message();
    return result;
  }

  result.readable = true;

  std::string_view content{read_result.value()};
  std::uint64_t line_number = 0;
  std::uint64_t first_malformed_line = 0;
  while (!content.empty()) {
    const auto newline = content.find('\n');
    const auto line = content.substr(0, newline);
    ++line_number;
    if (!is_blank_line(line)) {
      const auto parsed = nlohmann::json::parse(line, nullptr, false);
      if (!parsed.is_discarded()) {
        result.records.push_back(std::move(parsed));
      } else {
        result.has_malformed = true;
        ++result.malformed_line_count;
        if (first_malformed_line == 0) {
          first_malformed_line = line_number;
        }
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    content.remove_prefix(newline + 1);
  }

  if (result.has_malformed) {
    result.error_message = "malformed JSONL in '" + entry +
        "': " + std::to_string(result.malformed_line_count) +
        " unparseable line(s), first at line " +
        std::to_string(first_malformed_line);
  }

  return result;
}

JsonReadResult read_json_entry(const std::filesystem::path& package_path,
                               const std::string& entry) {
  JsonReadResult result;

  const auto layout = svp::package::read_package_layout(package_path);
  if (!layout.has_value()) {
    result.error_message = layout.error_message();
    return result;
  }

  result.present = layout.value().has_entry(entry);
  if (!result.present) {
    return result;
  }

  const auto read_result = svp::package::read_package_entry(package_path, entry);
  if (!read_result.has_value()) {
    result.error_message = read_result.error_message();
    return result;
  }

  result.readable = true;
  result.value = nlohmann::json::parse(read_result.value(), nullptr, false);
  if (result.value.is_discarded()) {
    result.error_message = "JSON parse failed";
    result.value = nlohmann::json{};
    return result;
  }

  result.parsed = true;
  return result;
}

}  // namespace svp::query
