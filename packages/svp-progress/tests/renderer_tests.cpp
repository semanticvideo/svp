#include "svp/progress/renderer.hpp"

#include <cassert>
#include <sstream>

int main() {
  using svp::progress::Event;
  using svp::progress::EventKind;

  std::ostringstream plain;
  auto plain_sink = svp::progress::make_sink(
      svp::progress::Mode::plain, plain, false);
  plain_sink->emit(Event{.kind = EventKind::progress,
                         .stage_id = "download",
                         .stage_label = "Download",
                         .current = 1,
                         .total = 2,
                         .unit = "files",
                         .scope_id = "one",
                         .scope_label = "Model One"});
  assert(plain.str() ==
         "[Model One] Download  [################----------------]  50%  1/2 files\n");

  std::ostringstream json;
  auto json_sink = svp::progress::make_sink(
      svp::progress::Mode::json, json, false);
  json_sink->emit(Event{.kind = EventKind::completed,
                        .stage_id = "models",
                        .stage_label = "Models",
                        .message = "ready"});
  assert(json.str().find("\"stage\":\"models\"") != std::string::npos);
  assert(json.str().find("\"stage_label\":\"Models\"") != std::string::npos);

  assert(json.str().find("\"t_ms\"") == std::string::npos);
  assert(json.str().find("\"seq\"") == std::string::npos);

  std::ostringstream timed_json;
  auto timed_json_sink = svp::progress::make_sink(
      svp::progress::Mode::json, timed_json, false);
  timed_json_sink->emit(Event{.kind = EventKind::started,
                              .stage_id = "models",
                              .stage_label = "Models",
                              .t_ms = 1234,
                              .seq = 7});
  assert(timed_json.str().find("\"t_ms\":1234") != std::string::npos);
  assert(timed_json.str().find("\"seq\":7") != std::string::npos);

  std::ostringstream timed_plain;
  auto timed_plain_sink = svp::progress::make_sink(
      svp::progress::Mode::plain, timed_plain, false);
  timed_plain_sink->emit(Event{.kind = EventKind::started,
                               .stage_id = "models",
                               .stage_label = "Models",
                               .t_ms = 1234,
                               .seq = 7});
  assert(timed_plain.str() == "Models  [working]\n");

  assert(svp::progress::parse_mode("auto") == svp::progress::Mode::auto_);
  assert(!svp::progress::parse_mode("invalid"));
  return 0;
}
