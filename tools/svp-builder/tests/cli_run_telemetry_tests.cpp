// The --run-report writer of a build another Mac coordinated
// (build --coordinators): that Mac's report stays as it is.

#include "cli/cli_run_telemetry.hpp"

#include "svp/builder/build_pipeline.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

std::string read(const fs::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
  const fs::path dir =
      fs::temp_directory_path() / ("svp-telemetry-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path report = dir / "run-report.json";

  {
    CliRunTelemetry telemetry("build", nullptr, report);
    std::ofstream(report) << "{\"written_by\":\"the coordinating Mac\"}";
    telemetry.leave_report_to_other_mac();
    require(telemetry.finish(0) == 0, "a build another Mac coordinated succeeds");
  }
  require(read(report) == "{\"written_by\":\"the coordinating Mac\"}",
          "the other Mac's report is left as it is");

  {
    CliRunTelemetry telemetry("build", nullptr, report);
    require(telemetry.finish(0) == 0, "a local build writes its report");
  }
  require(read(report).find("the coordinating Mac") == std::string::npos,
          "a local build's report replaces the file");

  fs::remove_all(dir);
  std::cout << "cli run telemetry tests passed\n";
  return 0;
}
