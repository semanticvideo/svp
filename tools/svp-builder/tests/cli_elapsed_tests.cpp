#include "cli/cli_elapsed.hpp"
#include "cli/cli_completion.hpp"

#include <cassert>
#include <chrono>

namespace {

void test_elapsed_duration_seconds() {
  using namespace std::chrono;
  assert(format_elapsed_duration(seconds(0)) == "0s");
  assert(format_elapsed_duration(seconds(45)) == "45s");
}

void test_elapsed_duration_minutes() {
  using namespace std::chrono;
  assert(format_elapsed_duration(minutes(2) + seconds(3)) == "2m 03s");
  assert(format_elapsed_duration(minutes(59) + seconds(59)) == "59m 59s");
}

void test_elapsed_duration_hours() {
  using namespace std::chrono;
  assert(format_elapsed_duration(hours(1) + minutes(4) + seconds(22)) ==
         "1h 04m 22s");
  assert(format_elapsed_duration(hours(2) + seconds(5)) == "2h 05s");
}

void test_completion_formats_build_interlace_and_transport_artifacts() {
  using namespace std::chrono;
  const std::filesystem::path output = "/tmp/result.bin";
  assert(build_artifact_label("svp") == kSvpArtifactLabel);
  assert(build_artifact_label("svpi") == kSvpiArtifactLabel);
  assert(build_artifact_label("embedded-svpi") ==
         kEmbeddedSvpiTransportArtifactLabel);
  assert(format_cli_completion(
             kSvpArtifactLabel, "created", output, seconds(45)) ==
         "SVP created in 45s: /tmp/result.bin");
  assert(format_cli_completion(
             kSvpiArtifactLabel, "created", output,
             minutes(2) + seconds(3)) ==
         "SVPI created in 2m 03s: /tmp/result.bin");
  assert(format_cli_completion(
             kEmbeddedSvpiTransportArtifactLabel, "created", output,
             hours(1) + seconds(5)) ==
         "Embedded SVPI transport created in 1h 05s: /tmp/result.bin");
  assert(format_cli_completion(
             kSvpiArtifactLabel, "extracted", output, seconds(9)) ==
         "SVPI extracted in 9s: /tmp/result.bin");
  assert(format_cli_completion(
             kInterlaceArtifactsLabel, "extracted", output, seconds(12)) ==
         "Interlace artifacts extracted in 12s: /tmp/result.bin");
  assert(format_cli_completion(
             kCleanIsoBmffContainerArtifactLabel, "created", output,
             seconds(7)) ==
         "Clean ISO BMFF container created in 7s: /tmp/result.bin");
}

}  // namespace

int main() {
  test_elapsed_duration_seconds();
  test_elapsed_duration_minutes();
  test_elapsed_duration_hours();
  test_completion_formats_build_interlace_and_transport_artifacts();
  return 0;
}
