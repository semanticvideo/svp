#pragma once

#include <functional>
#include <string>
#include <vector>

namespace svp::models::tool {

struct ProcessResult {
  int exit_code = 0;
  std::string output;
};

ProcessResult run_process(
    const std::vector<std::string>& arguments,
    const std::function<void(const std::string&)>& line_callback = {});

}  // namespace svp::models::tool
