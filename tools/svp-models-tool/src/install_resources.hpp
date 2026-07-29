#pragma once

#include <filesystem>

namespace svp::models::tool {

struct InstallResourcePaths {
  std::filesystem::path catalog;
  std::filesystem::path bundle_inputs;
  std::filesystem::path reference_set;
  std::filesystem::path ppocr_lock;
  std::filesystem::path rfdetr_lock;
  std::filesystem::path prepare_script;
  std::filesystem::path workflow_script;
};

InstallResourcePaths write_install_resources(
    const std::filesystem::path& root);

}  // namespace svp::models::tool
