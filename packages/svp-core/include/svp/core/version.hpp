#pragma once

#include <string>
#include <string_view>

namespace svp::core {

inline constexpr std::string_view kSpecName = "SVP";
inline constexpr std::string_view kSpecVersion = "1.0";
inline constexpr std::string_view kSpecReleaseStage = "RC1";
inline constexpr std::string_view kToolVersion = "1.0.0-rc1";

[[nodiscard]] std::string spec_version_label();
[[nodiscard]] std::string tool_version_label(std::string_view tool_name);

}  // namespace svp::core

