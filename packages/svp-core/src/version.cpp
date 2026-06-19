#include "svp/core/version.hpp"

#include <string>

namespace svp::core {

std::string spec_version_label() {
  std::string label(kSpecName);
  label.push_back(' ');
  label += kSpecVersion;
  label.push_back(' ');
  label += kSpecReleaseStage;
  return label;
}

std::string tool_version_label(std::string_view tool_name) {
  std::string label(tool_name);
  label.push_back(' ');
  label += kToolVersion;
  label += " (";
  label += spec_version_label();
  label.push_back(')');
  return label;
}

}  // namespace svp::core

