#include "svp/builder/build_progress.hpp"
#include "svp/builder/progress_renderer.hpp"
#include "svp/builder/progress_timeline.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using svp::builder::ProgressEvent;
using svp::builder::ProgressStageId;

class CapturingSink final : public svp::builder::BuildProgressSink {
 public:
  void emit(const ProgressEvent& event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    events.push_back(event);
  }
  std::vector<ProgressEvent> events;

 private:
  std::mutex mutex_;
};

std::vector<nlohmann::json> parse_jsonl(const std::string& text) {
  std::vector<nlohmann::json> lines;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) lines.push_back(nlohmann::json::parse(line));
  }
  return lines;
}

std::vector<ProgressEvent> representative_events() {
  return {
      svp::builder::make_stage_started(ProgressStageId::media_probe, "probing"),
      svp::builder::make_stage_completed(ProgressStageId::media_probe),
      svp::builder::make_stage_progress(ProgressStageId::asr, 1, 4, "chunks"),
      svp::builder::make_warning(ProgressStageId::ocr, "low contrast"),
      svp::builder::make_artifact_written(ProgressStageId::package_write,
                                          "/tmp/out.svp", "package"),
      svp::builder::with_progress_scope(
          svp::builder::make_stage_started(ProgressStageId::validate),
          "clip1", "clip1.mov"),
      svp::builder::make_stage_failed(ProgressStageId::validate, "bad"),
  };
}

void test_concurrent_emitters_get_strictly_ordered_seq_and_monotonic_t_ms() {
  // Mirrors the audio and vision lanes emitting at the same time.
  constexpr int kThreads = 8;
  constexpr int kEventsPerThread = 250;
  std::ostringstream json_output;
  auto json_sink = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, json_output, false);
  auto capture = std::make_shared<CapturingSink>();
  auto timeline =
      svp::builder::make_timestamped_progress_sink({json_sink, capture});

  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&timeline, thread_index]() {
      const auto stage = thread_index % 2 == 0 ? ProgressStageId::asr
                                               : ProgressStageId::ocr;
      for (int index = 0; index < kEventsPerThread; ++index) {
        timeline->emit(svp::builder::make_stage_progress(
            stage, static_cast<std::uint64_t>(index), kEventsPerThread,
            "units"));
      }
    });
  }
  for (auto& thread : threads) thread.join();

  constexpr std::uint64_t kTotal =
      static_cast<std::uint64_t>(kThreads) * kEventsPerThread;
  assert(timeline->emitted_count() == kTotal);

  const auto lines = parse_jsonl(json_output.str());
  assert(lines.size() == kTotal);
  std::int64_t previous_t_ms = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    assert(lines[index].at("seq").get<std::uint64_t>() == index + 1);
    const auto t_ms = lines[index].at("t_ms").get<std::int64_t>();
    assert(t_ms >= previous_t_ms);
    previous_t_ms = t_ms;
  }

  // Every downstream sink sees the same stamped order.
  assert(capture->events.size() == kTotal);
  for (std::size_t index = 0; index < capture->events.size(); ++index) {
    assert(capture->events[index].seq == index + 1);
    assert(capture->events[index].t_ms ==
           lines[index].at("t_ms").get<std::int64_t>());
  }
}

void test_t_ms_is_measured_from_origin() {
  constexpr auto kOriginOffset = std::chrono::seconds(5);
  auto capture = std::make_shared<CapturingSink>();
  auto timeline = svp::builder::make_timestamped_progress_sink(
      {capture}, std::chrono::steady_clock::now() - kOriginOffset);
  timeline->emit(svp::builder::make_stage_started(ProgressStageId::asr));
  assert(capture->events.size() == 1);
  assert(*capture->events.front().t_ms >=
         std::chrono::duration_cast<std::chrono::milliseconds>(kOriginOffset)
             .count());
  assert(timeline->elapsed_ms() >= *capture->events.front().t_ms);
}

void test_json_timing_fields_are_additive() {
  std::ostringstream untimed_output;
  auto untimed = svp::builder::make_progress_sink(
      svp::builder::ProgressMode::json, untimed_output, false);
  std::ostringstream timed_output;
  auto timed = svp::builder::make_timestamped_progress_sink(
      {svp::builder::make_progress_sink(svp::builder::ProgressMode::json,
                                        timed_output, false)});
  for (const auto& event : representative_events()) {
    untimed->emit(event);
    timed->emit(event);
  }
  const auto untimed_lines = parse_jsonl(untimed_output.str());
  auto timed_lines = parse_jsonl(timed_output.str());
  assert(untimed_lines.size() == timed_lines.size());
  for (std::size_t index = 0; index < timed_lines.size(); ++index) {
    assert(!untimed_lines[index].contains("t_ms"));
    assert(!untimed_lines[index].contains("seq"));
    assert(timed_lines[index].contains("t_ms"));
    assert(timed_lines[index].contains("seq"));
    timed_lines[index].erase("t_ms");
    timed_lines[index].erase("seq");
    assert(timed_lines[index] == untimed_lines[index]);
  }
}

void test_human_rendering_is_unchanged_by_timeline() {
  for (const bool is_tty : {false, true}) {
    for (const auto mode : {svp::builder::ProgressMode::plain,
                            svp::builder::ProgressMode::auto_}) {
      std::ostringstream direct_output;
      auto direct = svp::builder::make_progress_sink(mode, direct_output, is_tty);
      std::ostringstream timed_output;
      auto timed = svp::builder::make_timestamped_progress_sink(
          {svp::builder::make_progress_sink(mode, timed_output, is_tty)});
      for (const auto& event : representative_events()) {
        direct->emit(event);
        timed->emit(event);
      }
      assert(!direct_output.str().empty());
      assert(direct_output.str() == timed_output.str());
    }
  }
}

void test_scoped_events_keep_scope_through_timeline() {
  auto capture = std::make_shared<CapturingSink>();
  auto timeline = svp::builder::make_timestamped_progress_sink({capture});
  auto scoped =
      svp::builder::make_scoped_progress_sink(timeline, "item-1", "clip.mov");
  scoped->emit(svp::builder::make_stage_started(ProgressStageId::batch_item));
  assert(capture->events.size() == 1);
  assert(capture->events.front().scope_id == "item-1");
  assert(capture->events.front().scope_label == "clip.mov");
  assert(capture->events.front().seq == 1u);
}

}  // namespace

int main() {
  test_concurrent_emitters_get_strictly_ordered_seq_and_monotonic_t_ms();
  test_t_ms_is_measured_from_origin();
  test_json_timing_fields_are_additive();
  test_human_rendering_is_unchanged_by_timeline();
  test_scoped_events_keep_scope_through_timeline();
  return 0;
}
