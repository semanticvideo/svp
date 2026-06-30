#pragma once

#include <string_view>

namespace svp::package {

enum class SvpiForbiddenMediaReason {
  none,
  primary_media,
  replayable_derivative,
};

[[nodiscard]] SvpiForbiddenMediaReason
classify_svpi_entry(std::string_view entry_path) noexcept;

[[nodiscard]] bool is_svpi_entry_forbidden(std::string_view entry_path) noexcept;

}  // namespace svp::package
