#pragma once

#include "svp/models/manifest.hpp"

#include <filesystem>
#include <string>

namespace svp::models {

struct OnnxSessionOptions {
  std::string execution_provider = "cpu";
};

class OnnxSession {
 public:
  OnnxSession() = default;

  [[nodiscard]] static bool is_available();

  [[nodiscard]] static OnnxSession load(const ModelBundleManifest& manifest,
                                        const std::filesystem::path& bundle_root,
                                        const OnnxSessionOptions& options);
};

}  // namespace svp::models
