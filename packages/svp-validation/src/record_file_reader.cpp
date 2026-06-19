#include "record_file_reader.hpp"

#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace svp::validation {

JsonLinesReadResult read_json_lines_from_package(const std::filesystem::path& package_path,
                                                 const std::string& entry) {
  const auto entry_result = svp::package::read_package_entry(package_path, entry);
  if (!entry_result.has_value()) {
    return JsonLinesReadResult{.error_message = entry_result.error_message()};
  }

  JsonLinesReadResult result;
  std::istringstream input{entry_result.value()};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) {
      continue;
    }

    try {
      result.records.push_back(JsonLineRecord{
          .value = nlohmann::json::parse(line),
          .line = line_number,
      });
    } catch (const nlohmann::json::exception& error) {
      return JsonLinesReadResult{
          .error_message = "line " + std::to_string(line_number) + ": " + error.what(),
      };
    }
  }

  return result;
}

}  // namespace svp::validation
