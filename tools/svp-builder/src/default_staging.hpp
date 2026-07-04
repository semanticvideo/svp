#pragma once

#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
#include <stdexcept>

namespace svp::builder {

inline std::filesystem::path make_default_staging_dir() {
  const auto base = std::filesystem::temp_directory_path();
  std::random_device random;
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();

  for (int attempt = 0; attempt < 32; ++attempt) {
    std::ostringstream name;
    name << "svp-builder-" << ticks << "-" << random() << "-" << attempt;
    auto path = base / name.str();
    if (!std::filesystem::exists(path)) {
      return path;
    }
  }

  throw std::runtime_error("could not allocate a default staging directory");
}

}  // namespace svp::builder
