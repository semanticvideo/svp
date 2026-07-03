#include "svp/builder/progress_renderer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
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
  const std::string_view label = progress_stage_label(event.stage_id);
  switch (event.kind) {
    case ProgressEventKind::stage_started:
      return std::string(label) + "  [working]";
    case ProgressEventKind::stage_completed:
      return std::string(label) + "  [################################] 100%";
    case ProgressEventKind::stage_failed:
      return std::string(label) + "  FAILED";
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
      return std::string(label) + "  WARNING: " + event.message;
    case ProgressEventKind::artifact_written:
      return std::string(label) + "  wrote " + event.artifact_path.string();
  }
  return std::string(label);
}

class PlainProgressSink : public BuildProgressSink {
 public:
  explicit PlainProgressSink(std::ostream& stream) : stream_(stream) {}

  void emit(const ProgressEvent& event) override {
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
};

class JsonProgressSink : public BuildProgressSink {
 public:
  explicit JsonProgressSink(std::ostream& stream) : stream_(stream) {}

  void emit(const ProgressEvent& event) override {
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
    stream_ << j.dump() << '\n';
  }

 private:
  std::ostream& stream_;
};

class TtyProgressSink : public BuildProgressSink {
 public:
  explicit TtyProgressSink(std::ostream& stream, int terminal_fd)
      : stream_(stream),
        use_color_(!no_color_env()),
        terminal_fd_(terminal_fd >= 0 ? terminal_fd : STDOUT_FILENO) {}

  void emit(const ProgressEvent& event) override {
    const std::string_view label = progress_stage_label(event.stage_id);

    if (event.kind == ProgressEventKind::stage_progress) {
      write_progress_row(label, event);
    } else if (event.kind == ProgressEventKind::stage_started) {
      write_started_row(label, event);
    } else if (event.kind == ProgressEventKind::stage_completed) {
      write_completed_row(label, event);
    } else if (event.kind == ProgressEventKind::stage_failed) {
      write_failed_row(label, event);
    } else if (event.kind == ProgressEventKind::warning) {
      write_warning_row(label, event);
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

  std::size_t rendered_rows(const std::string& line) const {
    const int width = std::max(1, terminal_width());
    if (line.empty()) return 1;
    return (line.size() - 1) / static_cast<std::size_t>(width) + 1;
  }

  void clear_previous_rows() {
    stream_ << '\r' << clear_line();
    for (std::size_t row = 1; row < rendered_rows_; ++row) {
      stream_ << "\033[1A" << '\r' << clear_line();
    }
    stream_ << '\r';
  }

  void write_row(const std::string& line, bool newline) {
    clear_previous_rows();
    stream_ << line;
    rendered_rows_ = rendered_rows(line);
    if (newline) {
      stream_ << '\n';
      rendered_rows_ = 0;
    }
    stream_.flush();
  }

  void write_started_row(std::string_view label, const ProgressEvent& event) {
    std::ostringstream row;
    row << "  " << label << "  [working]";
    if (!event.message.empty()) row << "  " << event.message;
    write_row(row.str(), false);
  }

  void write_progress_row(std::string_view label, const ProgressEvent& event) {
    std::ostringstream row;
    row << "  " << label << "  ";
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
    write_row(row.str(), false);
  }

  void write_completed_row(std::string_view label, const ProgressEvent& event) {
    std::ostringstream row;
    if (use_color_) row << "\033[32m";
    row << "  " << label << "  [################################] 100%";
    if (use_color_) row << "\033[0m";
    if (!event.message.empty()) row << "  " << event.message;
    write_row(row.str(), true);
  }

  void write_failed_row(std::string_view label, const ProgressEvent& event) {
    std::ostringstream row;
    if (use_color_) row << "\033[31m";
    row << "  " << label << "  FAILED";
    if (use_color_) row << "\033[0m";
    if (!event.message.empty()) row << "  " << event.message;
    write_row(row.str(), true);
  }

  void write_warning_row(std::string_view label, const ProgressEvent& event) {
    std::ostringstream row;
    if (use_color_) row << "\033[33m";
    row << "  ! " << label << ": " << event.message;
    if (use_color_) row << "\033[0m";
    write_row(row.str(), true);
  }

  std::ostream& stream_;
  bool use_color_;
  int terminal_fd_;
  std::size_t rendered_rows_ = 0;
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
