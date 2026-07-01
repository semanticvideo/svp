#include "svp/builder/build_progress.hpp"
#include "svp/builder/progress_renderer.hpp"

#include <cassert>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void test_parse_progress_mode_valid() {
  assert(svp::builder::parse_progress_mode("auto") ==
         svp::builder::ProgressMode::auto_);
  assert(svp::builder::parse_progress_mode("plain") ==
         svp::builder::ProgressMode::plain);
  assert(svp::builder::parse_progress_mode("json") ==
         svp::builder::ProgressMode::json);
  assert(svp::builder::parse_progress_mode("none") ==
         svp::builder::ProgressMode::none);
}

void test_parse_progress_mode_invalid() {
  assert(!svp::builder::parse_progress_mode(""));
  assert(!svp::builder::parse_progress_mode("fancy"));
  assert(!svp::builder::parse_progress_mode("PLAIN"));
  assert(!svp::builder::parse_progress_mode("jsonl"));
}

void test_progress_mode_name_round_trip() {
  for (std::string_view name : {"auto", "plain", "json", "none"}) {
    auto mode = svp::builder::parse_progress_mode(name);
    assert(mode.has_value());
    assert(svp::builder::progress_mode_name(*mode) == name);
  }
}

void test_none_sink_produces_no_output() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::none, oss, false);
  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));
  assert(oss.str().empty());
}

void test_plain_sink_emits_deterministic_lines() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));
  sink->emit(svp::builder::make_artifact_written(
      svp::builder::ProgressStageId::package_write,
      "out/video.svp", "package"));
  sink->emit(svp::builder::make_warning(
      svp::builder::ProgressStageId::diarization, "fallback used"));
  sink->emit(svp::builder::make_stage_failed(
      svp::builder::ProgressStageId::validate, "validator exit 1"));

  const std::string output = oss.str();
  assert(output == "Media Probe  [working]  probing\n"
                   "Media Probe  [################################] 100%\n"
                   "Package Write  wrote out/video.svp\n"
                   "Diarization  WARNING: fallback used\n"
                   "Validation  FAILED  validator exit 1\n");
}

void test_plain_sink_empty_message_no_colon() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));

  const std::string output = oss.str();
  assert(output == "Media Probe  [working]\n"
                   "Media Probe  [################################] 100%\n");
}

void test_json_sink_emits_valid_jsonl() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));
  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));
  sink->emit(svp::builder::make_artifact_written(
      svp::builder::ProgressStageId::package_write,
      "out/video.svp", "package"));
  sink->emit(svp::builder::make_warning(
      svp::builder::ProgressStageId::diarization, "fallback used"));
  sink->emit(svp::builder::make_stage_failed(
      svp::builder::ProgressStageId::validate, "validator exit 1"));

  const std::string output = oss.str();
  std::istringstream lines(output);
  std::string line;
  int count = 0;
  while (std::getline(lines, line)) {
    assert(!line.empty());
    assert(line.front() == '{');
    assert(line.back() == '}');
    ++count;
  }
  assert(count == 5);
}

void test_json_sink_field_names() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));

  const std::string output = oss.str();
  assert(output.find("\"kind\"") != std::string::npos);
  assert(output.find("\"stage\"") != std::string::npos);
  assert(output.find("\"stage_label\"") != std::string::npos);
  assert(output.find("\"message\"") != std::string::npos);
  assert(output.find("\"stage_started\"") != std::string::npos);
  assert(output.find("\"media_probe\"") != std::string::npos);
  assert(output.find("\"Media Probe\"") != std::string::npos);
  assert(output.find("\"probing\"") != std::string::npos);
}

void test_json_sink_omits_empty_message() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_stage_completed(
      svp::builder::ProgressStageId::media_probe));

  const std::string output = oss.str();
  assert(output.find("\"message\"") == std::string::npos);
}

void test_json_sink_includes_artifact_path() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_artifact_written(
      svp::builder::ProgressStageId::package_write,
      "out/video.svp", "package"));

  const std::string output = oss.str();
  assert(output.find("\"artifact_path\"") != std::string::npos);
  assert(output.find("\"out/video.svp\"") != std::string::npos);
}

void test_auto_mode_non_tty_uses_plain() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::auto_, oss, false);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));

  const std::string output = oss.str();
  assert(output.find("Media Probe  [working]") != std::string::npos);
  assert(output.find('\r') == std::string::npos);
}

void test_auto_mode_tty_uses_ansi_progress() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::auto_, oss, true);

  sink->emit(svp::builder::make_stage_started(
      svp::builder::ProgressStageId::media_probe, "probing"));

  const std::string output = oss.str();
  assert(output.find("Media Probe") != std::string::npos);
  assert(output.find('\r') != std::string::npos);
  assert(output.find("\033[2K") != std::string::npos);
}

void test_plain_and_auto_non_tty_produce_same_output() {
  std::ostringstream plain_oss, auto_oss;

  auto plain_sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, plain_oss, false);
  auto auto_sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::auto_, auto_oss, false);

  const svp::builder::ProgressEvent events[] = {
      svp::builder::make_stage_started(
          svp::builder::ProgressStageId::media_probe, "probing"),
      svp::builder::make_stage_completed(
          svp::builder::ProgressStageId::media_probe),
      svp::builder::make_artifact_written(
          svp::builder::ProgressStageId::package_write,
          "out/video.svp", "package"),
      svp::builder::make_warning(
          svp::builder::ProgressStageId::diarization, "fallback used"),
      svp::builder::make_stage_failed(
          svp::builder::ProgressStageId::validate, "validator exit 1"),
  };

  for (const auto& event : events) {
    plain_sink->emit(event);
    auto_sink->emit(event);
  }

  assert(plain_oss.str() == auto_oss.str());
}

void test_plain_sink_stage_progress() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);

  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 3, 10, "chunks"));

  const std::string output = oss.str();
  assert(output.find("ASR") != std::string::npos);
  assert(output.find("chunks") != std::string::npos);
  assert(output.find("3/10") != std::string::npos);
  assert(output.find("30%") != std::string::npos);
  assert(output.find('#') != std::string::npos);
  assert(output.find('-') != std::string::npos);
}

void test_json_sink_stage_progress_fields() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, oss, false);

  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 3, 10, "chunks"));

  const std::string output = oss.str();
  assert(output.find("\"kind\"") != std::string::npos);
  assert(output.find("\"stage_progress\"") != std::string::npos);
  assert(output.find("\"current\"") != std::string::npos);
  assert(output.find("\"total\"") != std::string::npos);
  assert(output.find("\"fraction\"") != std::string::npos);
  assert(output.find("\"unit\"") != std::string::npos);
  assert(output.find("\"chunks\"") != std::string::npos);
  assert(output.find("\"asr\"") != std::string::npos);
}

void test_tty_sink_stage_progress_has_carriage_return() {
  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::auto_, oss, true);

  sink->emit(svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::color, 5, 15, "frames"));

  const std::string output = oss.str();
  assert(output.find('\r') != std::string::npos);
  assert(output.find("Color Observations") != std::string::npos);
  assert(output.find("frames") != std::string::npos);
}

void test_make_stage_progress_fraction() {
  const auto event = svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 5, 20, "chunks");
  assert(event.kind == svp::builder::ProgressEventKind::stage_progress);
  assert(event.current.has_value());
  assert(event.total.has_value());
  assert(event.fraction.has_value());
  assert(*event.current == 5);
  assert(*event.total == 20);
  assert(*event.fraction == 0.25);
  assert(event.unit == "chunks");
}

void test_make_stage_progress_zero_total() {
  const auto event = svp::builder::make_stage_progress(
      svp::builder::ProgressStageId::asr, 0, 0, "chunks");
  assert(event.current.has_value());
  assert(event.total.has_value());
  assert(!event.fraction.has_value());
}

void test_plain_sink_fraction_only_with_unit_no_crash() {
  svp::builder::ProgressEvent event;
  event.kind = svp::builder::ProgressEventKind::stage_progress;
  event.stage_id = svp::builder::ProgressStageId::asr;
  event.fraction = 0.5;
  event.unit = "chunks";

  std::ostringstream oss;
  auto sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::plain, oss, false);
  sink->emit(event);

  const std::string output = oss.str();
  assert(output.find("ASR") != std::string::npos);
  assert(output.find("50%") != std::string::npos);
  assert(output.find("chunks") == std::string::npos);
  assert(output.find("/0") == std::string::npos);
}

}  // namespace

int main() {
  test_parse_progress_mode_valid();
  test_parse_progress_mode_invalid();
  test_progress_mode_name_round_trip();
  test_none_sink_produces_no_output();
  test_plain_sink_emits_deterministic_lines();
  test_plain_sink_empty_message_no_colon();
  test_json_sink_emits_valid_jsonl();
  test_json_sink_field_names();
  test_json_sink_omits_empty_message();
  test_json_sink_includes_artifact_path();
  test_auto_mode_non_tty_uses_plain();
  test_auto_mode_tty_uses_ansi_progress();
  test_plain_and_auto_non_tty_produce_same_output();
  test_plain_sink_stage_progress();
  test_json_sink_stage_progress_fields();
  test_tty_sink_stage_progress_has_carriage_return();
  test_make_stage_progress_fraction();
  test_make_stage_progress_zero_total();
  test_plain_sink_fraction_only_with_unit_no_crash();

  return 0;
}
