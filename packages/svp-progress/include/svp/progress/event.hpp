#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::progress {

enum class EventKind {
  started,
  completed,
  failed,
  progress,
  warning,
  artifact_written,
};

std::string_view event_kind_name(EventKind kind);

struct Event {
  EventKind kind = EventKind::started;
  std::string stage_id;
  std::string stage_label;
  std::string message;
  std::filesystem::path artifact_path;
  std::optional<std::uint64_t> current;
  std::optional<std::uint64_t> total;
  std::optional<double> fraction;
  std::string unit;
  std::string scope_id;
  std::string scope_label;
  std::optional<std::int64_t> row_order;
};

}  // namespace svp::progress
