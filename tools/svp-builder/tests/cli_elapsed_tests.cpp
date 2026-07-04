#include "cli/cli_elapsed.hpp"

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

}  // namespace

int main() {
  test_elapsed_duration_seconds();
  test_elapsed_duration_minutes();
  test_elapsed_duration_hours();
  return 0;
}
