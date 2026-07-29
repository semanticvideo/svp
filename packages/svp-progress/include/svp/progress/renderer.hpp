#pragma once

#include "svp/progress/event.hpp"

#include <memory>
#include <optional>
#include <ostream>
#include <string_view>

namespace svp::progress {

enum class Mode {
  auto_,
  plain,
  json,
  none,
};

std::optional<Mode> parse_mode(std::string_view value);
std::string_view mode_name(Mode mode);

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void emit(const Event& event) = 0;
};

class NullSink final : public Sink {
 public:
  void emit(const Event& event) override;
};

std::shared_ptr<Sink> make_sink(Mode mode, std::ostream& stream,
                                bool is_tty, int terminal_fd = -1);

}  // namespace svp::progress
