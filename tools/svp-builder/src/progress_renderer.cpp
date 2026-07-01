#include "svp/builder/progress_renderer.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ostream>
#include <string>

namespace svp::builder {

namespace {

class PlainProgressSink : public BuildProgressSink {
 public:
  explicit PlainProgressSink(std::ostream& stream) : stream_(stream) {}

  void emit(const ProgressEvent& event) override {
    stream_ << '[' << progress_event_kind_name(event.kind) << "] "
            << progress_stage_id(event.stage_id);
    if (!event.message.empty()) {
      stream_ << ": " << event.message;
    }
    if (event.kind == ProgressEventKind::artifact_written &&
        !event.artifact_path.empty()) {
      stream_ << " " << event.artifact_path.string();
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
    stream_ << j.dump() << '\n';
  }

 private:
  std::ostream& stream_;
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
    ProgressMode mode, std::ostream& stream, bool is_tty) {
  switch (mode) {
    case ProgressMode::none:
      return std::make_shared<NullBuildProgressSink>();
    case ProgressMode::plain:
      return std::make_shared<PlainProgressSink>(stream);
    case ProgressMode::json:
      return std::make_shared<JsonProgressSink>(stream);
    case ProgressMode::auto_:
      if (is_tty) {
        return std::make_shared<PlainProgressSink>(stream);
      }
      return std::make_shared<PlainProgressSink>(stream);
  }
  return std::make_shared<NullBuildProgressSink>();
}

}  // namespace svp::builder
