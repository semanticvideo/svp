#pragma once

#include <filesystem>
#include <iosfwd>

namespace package_export {

struct ExportOptions {
  std::filesystem::path package;
  std::filesystem::path out;
  bool overwrite = false;
};

// `svp-inspector export`: validates the package, exports every layer into
// `out` (Package_Export_v1.md), and writes exactly one JSON result object to
// `result_stream`. Returns the process exit code.
[[nodiscard]] int run_export(const ExportOptions& options,
                             std::ostream& result_stream);

}  // namespace package_export
