#pragma once

#include "svp/builder/build_progress.hpp"

#include <memory>
#include <optional>
#include <ostream>
#include <string_view>

namespace svp::builder {

enum class ProgressMode {
  auto_,
  plain,
  json,
  none,
};

std::optional<ProgressMode> parse_progress_mode(std::string_view value);
std::string_view progress_mode_name(ProgressMode mode);

std::shared_ptr<BuildProgressSink> make_progress_sink(
    ProgressMode mode, std::ostream& stream, bool is_tty, int terminal_fd = -1);

}  // namespace svp::builder
