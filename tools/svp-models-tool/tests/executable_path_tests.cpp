#include "executable_path.hpp"

#include <cassert>
#include <filesystem>

int main() {
  const auto resolved =
      svp::models::tool::resolve_current_executable("bare-command-name");
  assert(resolved.is_absolute());
  assert(std::filesystem::is_regular_file(resolved));
  assert(resolved.filename() == "svp-models-executable-path-tests");
  return 0;
}
