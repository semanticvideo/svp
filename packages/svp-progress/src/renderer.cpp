#include "svp/progress/renderer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>

namespace svp::progress {
namespace {

constexpr int kBarWidth = 32;
constexpr int kFallbackTerminalWidth = 80;

bool no_color_env() {
  const char* env = std::getenv("NO_COLOR");
  return env != nullptr && env[0] != '\0';
}

std::string format_progress_bar(double fraction) {
  const int filled = static_cast<int>(fraction * kBarWidth);
  const int clamped = std::clamp(filled, 0, kBarWidth);
  std::string bar;
  bar.reserve(kBarWidth + 2);
  bar.push_back('[');
  for (int i = 0; i < clamped; ++i) bar.push_back('#');
  for (int i = clamped; i < kBarWidth; ++i) bar.push_back('-');
  bar.push_back(']');
  return bar;
}

std::string format_percent(double fraction) {
  const int pct = static_cast<int>(fraction * 100.0 + 0.5);
  std::ostringstream output;
  output << std::setw(3) << pct << '%';
  return output.str();
}

std::string scoped_label(const Event& event) {
  std::string label = event.stage_label;
  if (!event.scope_label.empty()) {
    label = "[" + event.scope_label + "] " + label;
  }
  return label;
}

std::string format_event_line(const Event& event) {
  const std::string label = scoped_label(event);
  switch (event.kind) {
    case EventKind::started:
      return label + "  [working]";
    case EventKind::completed:
      return label + "  [################################] 100%";
    case EventKind::failed:
      return label + "  FAILED";
    case EventKind::progress: {
      std::ostringstream output;
      output << label << "  ";
      if (event.fraction) {
        output << format_progress_bar(*event.fraction) << ' '
               << format_percent(*event.fraction);
      } else if (event.current && event.total && *event.total > 0) {
        const double fraction = static_cast<double>(*event.current) /
                                static_cast<double>(*event.total);
        output << format_progress_bar(fraction) << ' '
               << format_percent(fraction);
      } else {
        output << "[working]";
      }
      if (event.current && event.total) {
        output << "  " << *event.current << '/' << *event.total;
        if (!event.unit.empty()) output << ' ' << event.unit;
      }
      return output.str();
    }
    case EventKind::warning:
      return label + "  WARNING: " + event.message;
    case EventKind::artifact_written:
      return label + "  wrote " + event.artifact_path.string();
  }
  return label;
}

struct RowKey {
  std::string scope_id;
  std::optional<std::int64_t> row_order;
  std::string stage_id;

  bool operator<(const RowKey& other) const {
    if (scope_id != other.scope_id) return scope_id < other.scope_id;
    if (row_order != other.row_order) {
      if (!row_order) return false;
      if (!other.row_order) return true;
      return *row_order < *other.row_order;
    }
    return stage_id < other.stage_id;
  }
};

RowKey row_key(const Event& event) {
  return {
      .scope_id = event.scope_id,
      .row_order = event.row_order,
      .stage_id = event.stage_id,
  };
}

class PlainSink final : public Sink {
 public:
  explicit PlainSink(std::ostream& stream) : stream_(stream) {}

  void emit(const Event& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.kind == EventKind::artifact_written) return;
    stream_ << format_event_line(event);
    if (!event.message.empty() && event.kind != EventKind::warning) {
      stream_ << "  " << event.message;
    }
    stream_ << '\n';
  }

 private:
  std::ostream& stream_;
  std::mutex mutex_;
};

class JsonSink final : public Sink {
 public:
  explicit JsonSink(std::ostream& stream) : stream_(stream) {}

  void emit(const Event& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json value;
    value["kind"] = std::string(event_kind_name(event.kind));
    value["stage"] = event.stage_id;
    value["stage_label"] = event.stage_label;
    if (!event.message.empty()) value["message"] = event.message;
    if (event.kind == EventKind::artifact_written && !event.artifact_path.empty()) {
      value["artifact_path"] = event.artifact_path.string();
    }
    if (event.current) value["current"] = *event.current;
    if (event.total) value["total"] = *event.total;
    if (event.fraction) value["fraction"] = *event.fraction;
    if (!event.unit.empty()) value["unit"] = event.unit;
    if (!event.scope_id.empty()) value["scope_id"] = event.scope_id;
    if (!event.scope_label.empty()) value["scope_label"] = event.scope_label;
    stream_ << value.dump() << '\n';
  }

 private:
  std::ostream& stream_;
  std::mutex mutex_;
};

class TtySink final : public Sink {
 public:
  TtySink(std::ostream& stream, int terminal_fd)
      : stream_(stream),
        use_color_(!no_color_env()),
        terminal_fd_(terminal_fd >= 0 ? ::dup(terminal_fd) : -1),
        owns_terminal_fd_(terminal_fd_ >= 0) {
    if (terminal_fd_ < 0) {
      terminal_fd_ = terminal_fd >= 0 ? terminal_fd : STDOUT_FILENO;
    }
  }

  ~TtySink() override {
    if (owns_terminal_fd_) ::close(terminal_fd_);
  }

  void emit(const Event& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.kind == EventKind::progress || event.kind == EventKind::started) {
      active_rows_[row_key(event)] = active_row(event);
      redraw();
    } else if (event.kind == EventKind::completed || event.kind == EventKind::failed) {
      active_rows_.erase(row_key(event));
      history_then_active(final_row(event));
    } else if (event.kind == EventKind::warning) {
      history_then_active(warning_row(event));
    }
  }

 private:
  int terminal_width() const {
    winsize size{};
    if (ioctl(terminal_fd_, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
      return static_cast<int>(size.ws_col);
    }
    return kFallbackTerminalWidth;
  }

  std::size_t physical_rows(const std::string& line) const {
    const int width = std::max(1, terminal_width());
    return line.empty() ? 1 :
        (line.size() - 1) / static_cast<std::size_t>(width) + 1;
  }

  void write_text(std::string_view text) {
    if (!owns_terminal_fd_) {
      stream_ << text;
      return;
    }
    const char* data = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
      const ssize_t written = ::write(terminal_fd_, data, remaining);
      if (written < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (written == 0) break;
      data += written;
      remaining -= static_cast<std::size_t>(written);
    }
  }

  void clear_previous() {
    write_text("\r\033[2K");
    for (std::size_t row = 1; row < rendered_rows_; ++row) {
      write_text("\033[1A\r\033[2K");
    }
    write_text("\r");
  }

  std::vector<std::string> lines() const {
    std::vector<std::string> result;
    result.reserve(active_rows_.size());
    for (const auto& [key, line] : active_rows_) {
      (void)key;
      result.push_back(line);
    }
    return result;
  }

  void write_active() {
    rendered_rows_ = 0;
    const auto active = lines();
    for (std::size_t index = 0; index < active.size(); ++index) {
      if (index > 0) write_text("\n");
      write_text(active[index]);
      rendered_rows_ += physical_rows(active[index]);
    }
    if (!owns_terminal_fd_) stream_.flush();
  }

  void redraw() {
    clear_previous();
    write_active();
  }

  void history_then_active(const std::string& history) {
    clear_previous();
    write_text(history);
    write_text("\n");
    rendered_rows_ = 0;
    write_active();
  }

  std::string active_row(const Event& event) const {
    std::ostringstream row;
    row << "  " << scoped_label(event) << "  ";
    if (event.kind == EventKind::started) {
      row << "[working]";
    } else if (event.fraction) {
      row << format_progress_bar(*event.fraction) << ' '
          << format_percent(*event.fraction);
    } else if (event.current && event.total && *event.total > 0) {
      const double fraction = static_cast<double>(*event.current) /
                              static_cast<double>(*event.total);
      row << format_progress_bar(fraction) << ' ' << format_percent(fraction);
    } else {
      row << "[working]";
    }
    if (!event.unit.empty() && event.current && event.total) {
      row << "  " << *event.current << '/' << *event.total << ' ' << event.unit;
    }
    if (!event.message.empty()) row << "  " << event.message;
    return row.str();
  }

  std::string final_row(const Event& event) const {
    std::ostringstream row;
    if (use_color_) row << (event.kind == EventKind::completed ? "\033[32m" : "\033[31m");
    row << "  " << scoped_label(event) << "  ";
    if (event.kind == EventKind::completed) {
      row << "[################################] 100%";
    } else {
      row << "FAILED";
    }
    if (use_color_) row << "\033[0m";
    if (!event.message.empty()) row << "  " << event.message;
    return row.str();
  }

  std::string warning_row(const Event& event) const {
    std::ostringstream row;
    if (use_color_) row << "\033[33m";
    row << "  ! " << scoped_label(event) << ": " << event.message;
    if (use_color_) row << "\033[0m";
    return row.str();
  }

  std::ostream& stream_;
  bool use_color_;
  int terminal_fd_;
  bool owns_terminal_fd_;
  std::size_t rendered_rows_ = 0;
  std::map<RowKey, std::string> active_rows_;
  std::mutex mutex_;
};

}  // namespace

std::optional<Mode> parse_mode(std::string_view value) {
  if (value == "auto") return Mode::auto_;
  if (value == "plain") return Mode::plain;
  if (value == "json") return Mode::json;
  if (value == "none") return Mode::none;
  return std::nullopt;
}

std::string_view mode_name(Mode mode) {
  switch (mode) {
    case Mode::auto_:
      return "auto";
    case Mode::plain:
      return "plain";
    case Mode::json:
      return "json";
    case Mode::none:
      return "none";
  }
  return "unknown";
}

std::shared_ptr<Sink> make_sink(Mode mode, std::ostream& stream,
                                bool is_tty, int terminal_fd) {
  switch (mode) {
    case Mode::none:
      return std::make_shared<NullSink>();
    case Mode::plain:
      return std::make_shared<PlainSink>(stream);
    case Mode::json:
      return std::make_shared<JsonSink>(stream);
    case Mode::auto_:
      return is_tty ? std::shared_ptr<Sink>(
                          std::make_shared<TtySink>(stream, terminal_fd))
                    : std::shared_ptr<Sink>(std::make_shared<PlainSink>(stream));
  }
  return std::make_shared<NullSink>();
}

}  // namespace svp::progress
