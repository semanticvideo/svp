#include "previous_export.hpp"

#include "export_plan.hpp"
#include "export_summary.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>

namespace package_export {
namespace {

// export_format_version values whose directories this build may replace.
constexpr std::array<int, 1> kReplaceableExportFormatVersions{
    kExportFormatVersion};

bool is_real_directory(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::is_directory(status);
}

bool is_supported_version(const nlohmann::json& version) {
  if (!version.is_number_integer()) {
    return false;
  }
  const auto value = version.get<std::int64_t>();
  for (const auto supported : kReplaceableExportFormatVersions) {
    if (value == supported) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool is_previous_export(const std::filesystem::path& directory,
                        std::uint64_t max_summary_bytes) {
  const auto summary_path = directory / std::string{kSummaryFileName};
  std::error_code error;
  const auto status = std::filesystem::symlink_status(summary_path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    return false;
  }
  const auto size = std::filesystem::file_size(summary_path, error);
  if (error || size > max_summary_bytes) {
    return false;
  }
  if (!is_real_directory(directory / std::string{kLayersDirectoryName})) {
    return false;
  }

  std::ifstream input(summary_path, std::ios::binary);
  if (!input) {
    return false;
  }
  // Read at most one byte past the size just checked: a file that changed
  // after the check is refused instead of read without limit.
  std::string content(static_cast<std::size_t>(size) + 1, '\0');
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  const auto count = static_cast<std::uint64_t>(input.gcount());
  if (count != size) {
    return false;
  }
  content.resize(static_cast<std::size_t>(count));

  const auto summary = nlohmann::json::parse(content, nullptr, false);
  if (summary.is_discarded() || !summary.is_object()) {
    return false;
  }
  const auto format = summary.find("export_format");
  const auto version = summary.find("export_format_version");
  return format != summary.end() && format->is_string() &&
         format->get_ref<const std::string&>() == kExportFormatName &&
         version != summary.end() && is_supported_version(*version);
}

}  // namespace package_export
