#include "svp/builder/progress_renderer.hpp"

#include "svp/progress/event.hpp"

#include <utility>

namespace svp::builder {
namespace {

svp::progress::EventKind neutral_kind(ProgressEventKind kind) {
  switch (kind) {
    case ProgressEventKind::stage_started:
      return svp::progress::EventKind::started;
    case ProgressEventKind::stage_completed:
      return svp::progress::EventKind::completed;
    case ProgressEventKind::stage_failed:
      return svp::progress::EventKind::failed;
    case ProgressEventKind::stage_progress:
      return svp::progress::EventKind::progress;
    case ProgressEventKind::warning:
      return svp::progress::EventKind::warning;
    case ProgressEventKind::artifact_written:
      return svp::progress::EventKind::artifact_written;
  }
  return svp::progress::EventKind::failed;
}

svp::progress::Event neutral_event(const ProgressEvent& event) {
  return {
      .kind = neutral_kind(event.kind),
      .stage_id = std::string(progress_stage_id(event.stage_id)),
      .stage_label = std::string(progress_stage_label(event.stage_id)),
      .message = event.message,
      .artifact_path = event.artifact_path,
      .current = event.current,
      .total = event.total,
      .fraction = event.fraction,
      .unit = event.unit,
      .scope_id = event.scope_id,
      .scope_label = event.scope_label,
  };
}

class BuilderProgressAdapter final : public BuildProgressSink {
 public:
  explicit BuilderProgressAdapter(std::shared_ptr<svp::progress::Sink> sink)
      : sink_(std::move(sink)) {}

  void emit(const ProgressEvent& event) override {
    sink_->emit(neutral_event(event));
  }

 private:
  std::shared_ptr<svp::progress::Sink> sink_;
};

}  // namespace

std::optional<ProgressMode> parse_progress_mode(std::string_view value) {
  return svp::progress::parse_mode(value);
}

std::string_view progress_mode_name(ProgressMode mode) {
  return svp::progress::mode_name(mode);
}

std::shared_ptr<BuildProgressSink> make_progress_sink(
    ProgressMode mode, std::ostream& stream, bool is_tty, int terminal_fd) {
  return std::make_shared<BuilderProgressAdapter>(
      svp::progress::make_sink(mode, stream, is_tty, terminal_fd));
}

}  // namespace svp::builder
