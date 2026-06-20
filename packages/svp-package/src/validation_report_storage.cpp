#include "svp/package/validation_report_storage.hpp"

#include <filesystem>
#include <fstream>

namespace svp::package {

bool write_validation_report_to_staging(
    const std::filesystem::path& staging_dir,
    const nlohmann::json& validation_report) {
  try {
    const std::filesystem::path report_path =
        staging_dir / "provenance" / "validation.json";
    std::filesystem::create_directories(report_path.parent_path());

    std::ofstream output(report_path);
    if (!output) {
      return false;
    }
    output << validation_report.dump(2) << "\n";
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace svp::package
