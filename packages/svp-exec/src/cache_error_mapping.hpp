#pragma once

#include "svp/exec/cache_error.hpp"

#include <string>
#include <string_view>
#include <system_error>

namespace svp::exec::detail {

// Maps filesystem errors onto the RC2 §20.5.2 failure classes.
[[nodiscard]] CacheErrorCode cache_error_code_for(const std::error_code& error) noexcept;
[[nodiscard]] CacheError cache_error(const std::error_code& error, std::string_view context);
[[nodiscard]] CacheError cache_error(CacheErrorCode code, std::string message);

}  // namespace svp::exec::detail
