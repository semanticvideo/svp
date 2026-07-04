#include "svp/builder/progress_renderer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>

namespace svp::builder {

namespace {

bool no_color_env() {
  const char* env = std::getenv("NO_COLOR");
  return env != nullptr && env[0] != '\0';
}

constexpr int kBarWidth = 32;
constexpr int kFallbackTerminalWidth = 80;

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
  std::ostringstream oss;
  oss << std::setw(3) << pct << '%';
  return oss.str();
}

std::string format_stage_line(const ProgressEvent& event) {
  std::string label(progress_stage_label(event.stage_id));
  if (!event.scope_label.empty()) {
    label = "[" + event.scope_label + "] " + label;
  }
  switch (event.kind) {
    case ProgressEventKind::stage_started:
      return label + "  [working]";
    case ProgressEventKind::stage_completed:
      return label + "  [################################] 100%";
    case ProgressEventKind::stage_failed:
      return label + "  FAILED";
    case ProgressEventKind::stage_progress: {
      std::ostringstream oss;
      oss << label << "  ";
      if (event.fraction) {
        oss << format_progress_bar(*event.fraction) << ' '
            << format_percent(*event.fraction);
      } else if (event.current && event.total && *event.total > 0) {
        const double frac =
            static_cast<double>(*event.current) / static_cast<double>(*event.total);
        oss << format_progress_bar(frac) << ' ' << format_percent(frac);
      } else {
        oss << "[working]";
      }
      if (event.current && event.total) {
        oss << "  " << *event.current << '/' << *event.total;
        if (!event.unit.empty()) {
          oss << ' ' << event.unit;
        }
      }
      return oss.str();
    }
    case ProgressEventKind::warning:
      return label + "  WARNING: " + event.message;
    case ProgressEventKind::artifact_written:
      return label + "  wrote " + event.artifact_path.string();
  }
  return label;
}

struct ProgressRowKey {
  std::string scope_id;
  ProgressStageId stage_id;

  bool operator<(const ProgressRowKey& other) const {
    if (scope_id != other.scope_id) {
      return scope_id < other.scope_id;
    }
    return static_cast<int>(stage_id) < static_cast<int>(other.stage_id);
  }
};

ProgressRowKey row_key_for(const ProgressEvent& event) {
  return {.scope_id = event.scope_id, .stage_id = event.stage_id};
}

class PlainProgressSink : public BuildProgressSink {
 public:
  explicit PlainProgressSink(std::ostream& stream) : stream_(stream) {}

  void emit(const ProgressEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.kind == ProgressEventKind::artifact_written) {
      return;
    }
    stream_ << format_stage_line(event);
    if (!event.message.empty() &&
        event.kind != ProgressEventKind::warning) {
      stream_ << "  " << event.message;
    }
    stream_ << '\n';
  }

 private:
  std::ostream& stream_;
  std::mutex mutex_;
};

class JsonProgressSink : public BuildProgressSink {
 public:
  explicit JsonProgressSink(std::ostream& stream) : stream_(stream) {}

  void emit(const ProgressEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json j;
    j["kind"] = std::string(progress_event_kind_name(event.kind));
    j["stage"] = std::string(progress_stage_id(event.stage_id));
    j["stage_label"] = std::string(progress_stage_label(event.stage_id));
    if (!event.message.empty()) {
      j["message"] = event.message;
    }
    if (event.kind == ProgressEventKind::artifact_written &&
        !event.artifact_path.empty()) {
      j["artifact_path"] = event.artifact_path.string();
    }
    if (event.current) {
      j["current"] = *event.current;
    }
    if (event.total) {
      j["total"] = *event.total;
    }
    if (event.fraction) {
      j["fraction"] = *event.fraction;
    }
    if (!event.unit.empty()) {
      j["unit"] = event.unit;
    }
    if (!event.scope_id.empty()) {
      j["scope_id"] = event.scope_id;
    }
    if (!event.scope_label.empty()) {
      j["scope_label"] = event.scope_label;
    }
    stream_ << j.dump() << '\n';
  }

 private:
  std::ostream& stream_;
  std::mutex mutex_;
};

class TtyProgressSink : public BuildProgressSink {
 public:
  explicit TtyProgressSink(std::ostream& stream, int terminal_fd)
      : stream_(stream),
        use_color_(!no_color_env()),
        terminal_fd_(terminal_fd >= 0 ? ::dup(terminal_fd) : -1),
        owns_terminal_fd_(terminal_fd_ >= 0) {
    if (terminal_fd_ < 0) {
      terminal_fd_ = terminal_fd >= 0 ? terminal_fd : STDOUT_FILENO;
    }
  }

  ~TtyProgressSink() override {
    if (owns_terminal_fd_) {
      ::close(terminal_fd_);
    }
  }

  void emit(const ProgressEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.kind == ProgressEventKind::stage_progress) {
      update_active_row(event);
    } else if (event.kind == ProgressEventKind::stage_started) {
      update_active_row(event);
    } else if (event.kind == ProgressEventKind::stage_completed) {
      finalize_row(event, true);
    } else if (event.kind == ProgressEventKind::stage_failed) {
      finalize_row(event, false);
    } else if (event.kind == ProgressEventKind::warning) {
      write_warning_row(event);
    } else if (event.kind == ProgressEventKind::artifact_written) {
      return;
    }
  }

 private:
  static std::string clear_line() { return "\033[2K"; }

  int terminal_width() const {
    winsize size{};
    if (ioctl(terminal_fd_, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
      return static_cast<int>(size.ws_col);
    }
    return kFallbackTerminalWidth;
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
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (written == 0) {
        break;
      }
      data += written;
      remaining -= static_cast<std::size_t>(written);
    }
  }

  void flush_output() {
    if (!owns_terminal_fd_) {
      stream_.flush();
    }
  }

  std::size_t rendered_rows(const std::string& line) const {
    const int width = std::max(1, terminal_width());
    if (line.empty()) return 1;
    return (line.size() - 1) / static_cast<std::size_t>(width) + 1;
  }

  void clear_previous_rows() {
    write_text("\r");
    write_text(clear_line());
    for (std::size_t row = 1; row < rendered_rows_; ++row) {
      write_text("\033[1A");
      write_text("\r");
      write_text(clear_line());
    }
    write_text("\r");
  }

  void write_rows(const std::vector<std::string>& lines, bool newline) {
    clear_previous_rows();
    rendered_rows_ = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i > 0) {
        write_text("\n");
      }
      write_text(lines[i]);
      rendered_rows_ += rendered_rows(lines[i]);
    }
    if (newline) {
      write_text("\n");
      rendered_rows_ = 0;
    }
    flush_output();
  }

  std::vector<std::string> active_lines() const {
    std::vector<std::string> lines;
    lines.reserve(active_rows_.size());
    for (const auto& [key, line] : active_rows_) {
      (void)key;
      lines.push_back(line);
    }
    return lines;
  }

  void redraw_active_rows() {
    write_rows(active_lines(), false);
  }

  void write_history_line_then_active(const std::string& history_line) {
    clear_previous_rows();
    write_text(history_line);
    write_text("\n");
    rendered_rows_ = 0;
    const std::vector<std::string> lines = active_lines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (i > 0) {
        write_text("\n");
      }
      write_text(lines[i]);
      rendered_rows_ += rendered_rows(lines[i]);
    }
    flush_output();
  }

  static std::string scoped_label(const ProgressEvent& event) {
    std::string label(progress_stage_label(event.stage_id));
    if (!event.scope_label.empty()) {
      label = "[" + event.scope_label + "] " + label;
    }
    return label;
  }

  std::string started_row(const ProgressEvent& event) const {
    std::ostringstream row;
    row << "  " << scoped_label(event) << "  [working]";
    if (!event.message.empty()) row << "  " << event.message;
    return row.str();
  }

  std::string progress_row(const ProgressEvent& event) const {
    std::ostringstream row;
    row << "  " << scoped_label(event) << "  ";
    if (event.fraction) {
      row << format_progress_bar(*event.fraction) << ' '
          << format_percent(*event.fraction);
    } else if (event.current && event.total && *event.total > 0) {
      const double frac =
          static_cast<double>(*event.current) / static_cast<double>(*event.total);
      row << format_progress_bar(frac) << ' ' << format_percent(frac);
    } else {
      row << "[working]";
    }
    if (!event.unit.empty() && event.current && event.total) {
      row << "  " << *event.current << '/' << *event.total << ' ' << event.unit;
    }
    if (!event.message.empty()) row << "  " << event.message;
    return row.str();
  }

  std::string completed_row(const ProgressEvent& event) const {
    std::ostringstream row;
    if (use_color_) row << "\033[32m";
    row << "  " << scoped_label(event) << "  [################################] 100%";
    if (use_color_) row << "\033[0m";
    if (!event.message.empty()) row << "  " << event.message;
    return row.str();
  }

  std::string failed_row(const ProgressEvent& event) const {
    std::ostringstream row;
    if (use_color_) row << "\033[31m";
    row << "  " << scoped_label(event) << "  FAILED";
    if (use_color_) row << "\033[0m";
    if (!event.message.empty()) row << "  " << event.message;
    return row.str();
  }

  void update_active_row(const ProgressEvent& event) {
    active_rows_[row_key_for(event)] =
        event.kind == ProgressEventKind::stage_started
            ? started_row(event)
            : progress_row(event);
    redraw_active_rows();
  }

  void finalize_row(const ProgressEvent& event, bool completed) {
    active_rows_.erase(row_key_for(event));
    write_history_line_then_active(
        completed ? completed_row(event) : failed_row(event));
  }

  void write_warning_row(const ProgressEvent& event) {
    std::ostringstream row;
    if (use_color_) row << "\033[33m";
    row << "  ! " << scoped_label(event) << ": " << event.message;
    if (use_color_) row << "\033[0m";
    write_history_line_then_active(row.str());
  }

  std::ostream& stream_;
  bool use_color_;
  int terminal_fd_;
  bool owns_terminal_fd_;
  std::size_t rendered_rows_ = 0;
  std::map<ProgressRowKey, std::string> active_rows_;
  std::mutex mutex_;
};

}  // namespace

std::optional<ProgressMode> parse_progress_mode(std::string_view value) {
  if (value == "auto") return ProgressMode::auto_;
  if (value == "plain") return ProgressMode::plain;
  if (value == "json") return ProgressMode::json;
  if (value == "none") return ProgressMode::none;
  return std::nullopt;
}

std::string_view progress_mode_name(ProgressMode mode) {
  switch (mode) {
    case ProgressMode::auto_:
      return "auto";
    case ProgressMode::plain:
      return "plain";
    case ProgressMode::json:
      return "json";
    case ProgressMode::none:
      return "none";
  }
  return "unknown";
}

std::shared_ptr<BuildProgressSink> make_progress_sink(
    ProgressMode mode, std::ostream& stream, bool is_tty, int terminal_fd) {
  switch (mode) {
    case ProgressMode::none:
      return std::make_shared<NullBuildProgressSink>();
    case ProgressMode::plain:
      return std::make_shared<PlainProgressSink>(stream);
    case ProgressMode::json:
      return std::make_shared<JsonProgressSink>(stream);
    case ProgressMode::auto_:
      if (is_tty) {
        return std::make_shared<TtyProgressSink>(stream, terminal_fd);
      }
      return std::make_shared<PlainProgressSink>(stream);
  }
  return std::make_shared<NullBuildProgressSink>();
}

}  // namespace svp::builder
