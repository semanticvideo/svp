#pragma once

#include <filesystem>

namespace svp::models {

[[nodiscard]] std::filesystem::path default_cache_root();
[[nodiscard]] std::filesystem::path model_cache_root();

}  // namespace svp::models
