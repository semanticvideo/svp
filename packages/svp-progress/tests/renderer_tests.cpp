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

  assert(svp::progress::parse_mode("auto") == svp::progress::Mode::auto_);
  assert(!svp::progress::parse_mode("invalid"));
  return 0;
}
