#include "engine/dispatch_scratch.hpp"

#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace svp::builder::engine {

DispatchScratch::DispatchScratch() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "svp-dispatch-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    throw std::runtime_error("cannot create a scratch directory for dispatched vision tasks");
  }
  path_ = pattern;
}

DispatchScratch::~DispatchScratch() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

}  // namespace svp::builder::engine
