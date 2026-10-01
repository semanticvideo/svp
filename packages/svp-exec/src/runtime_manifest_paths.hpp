#pragma once

#include <string_view>

namespace svp::exec::detail {

// A runtime manifest path must name a file inside the runtime root wherever
// that root is: relative, '/'-separated, with no empty, "." or ".." segment
// and no backslash or NUL. Workers resolve these paths under their own cache
// directory, so this rule is what keeps a manifest from reaching outside it.
[[nodiscard]] bool is_safe_runtime_path(std::string_view path) noexcept;

}  // namespace svp::exec::detail
