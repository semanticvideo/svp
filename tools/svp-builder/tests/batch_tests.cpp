// Whole-video batches (M6): the video.build parameters and package record
// are strict; the batch dispatcher gives each Mac one video at a time and
// requeues what a busy or unavailable Mac gives back; build-batch names,
// refuses, rebuilds, and keeps outputs as documented; and an in-process
// executor reports its running tasks to the Mac's local load.

#include "batch/batch_dispatch.hpp"
#include "batch/build_batch.hpp"
#include "engine/local_load_executor.hpp"

#include "svp/builder/video_build_parameters.hpp"
#include "svp/exec/output_digest.hpp"
#include "svp/models/thread_plan.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace batch = svp::builder::batch;
namespace engine = svp::builder::engine;
namespace exec = svp::exec;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "Test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Function>
void require_throws(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  require(false, message + ": nothing was thrown");
}

struct TemporaryDirectory {
  std::filesystem::path path;
  explicit TemporaryDirectory(const std::string& name) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / (name + "-XXXXXX")).string();
    require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp");
    path = pattern;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

batch::VideoBuildParameters sample_parameters() {
  batch::VideoBuildParameters parameters;
  parameters.output_format = batch::VideoOutputFormat::svpi;
  parameters.source_name = "clip one.mp4";
  parameters.performance.ocr_performance_profile = "fast";
  parameters.visual_tracking_quality = "off";
  parameters.force_single_speaker = true;
  parameters.compute_full_blake3 = false;
  parameters.distributed = true;
  parameters.require_workers = 2;
  parameters.run_report = true;
  parameters.ffmpeg_build = exec::blake3_prefixed(exec::blake3_digest(std::string_view("ffmpeg")));
  parameters.thread_plan =
      svp::models::thread_plan_to_json(svp::models::resolve_local_thread_plan({6}, 4));
  return parameters;
}

void test_parameters_round_trip_and_are_strict() {
  const batch::VideoBuildParameters parameters = sample_parameters();
  const nlohmann::json json = batch::video_build_parameters_to_json(parameters);
  require(batch::video_build_parameters_from_json(json) == parameters, "parameters round trip");
  require(!batch::validate_video_build_parameters(json), "valid parameters pass the validator");

  const auto rejected = [&](const std::string& field, const nlohmann::json& value) {
    nlohmann::json changed = json;
    changed[field] = value;
    require(batch::validate_video_build_parameters(changed).has_value(),
            field + " = " + value.dump() + " is refused");
  };
  rejected("output_format", "mov");
  rejected("source_name", "../escape.mp4");
  rejected("source_name", "dir/clip.mp4");
  rejected("source_name", "");
  rejected("ocr_performance_profile", "turbo");
  rejected("visual_tracking_quality", "ultra");
  rejected("ffmpeg_build", "/usr/bin/ffmpeg");
  rejected("require_workers", -1);
  rejected("distributed", "yes");
  rejected("command", "rm -rf /");
  nlohmann::json missing = json;
  missing.erase("thread_plan");
  require(batch::validate_video_build_parameters(missing).has_value(),
          "a missing member is refused");
  rejected("thread_plan", nlohmann::json::object());
}

void test_package_record_round_trips() {
  const batch::VideoBuildPackageRecord record{
      .file_name = "clip one.svp",
      .blake3 = exec::blake3_digest(std::string_view("package")),
      .bytes = 1234};
  require(batch::decode_video_build_package_record(
              batch::encode_video_build_package_record(record)) == record,
          "package record round trips");
  require_throws([] { (void)batch::decode_video_build_package_record(R"({"file_name":"x"})"); },
                 "a record without its blob");
  require_throws(
      [] {
        (void)batch::decode_video_build_package_record(
            R"({"blob":{"blake3":"00","bytes":1},"file_name":"x"})");
      },
      "a record with a bad digest");
}

// A model ref whose bundle id names its model and its digest's prefix, as
// model bundle ids do (svp/models/model_id.hpp).
exec::TaskModelRef model_ref(const std::string& model_id) {
  constexpr std::size_t kBundleIdHashPrefixDigits = 12;
  const exec::Blake3Digest digest = exec::blake3_digest(std::string_view(model_id));
  return exec::TaskModelRef{
      .model_id = model_id,
      .model_bundle_id = model_id + "@1.0.0+blake3_" +
                         exec::blake3_hex(digest).substr(0, kBundleIdHashPrefixDigits),
      .bundle_blake3 = digest};
}

void test_spec_names_inputs_and_models() {
  const exec::TaskSpec spec = batch::make_video_build_spec(batch::VideoBuildSpecInput{
      .build_session_id = "batch_1",
      .task_id = "video_000001",
      .parameters = sample_parameters(),
      .source = exec::ArtifactRef{.blake3 = exec::blake3_digest(std::string_view("source")),
                                  .bytes = 6},
      .model_lock = exec::ArtifactRef{.blake3 = exec::blake3_digest(std::string_view("lock")),
                                      .bytes = 4},
      .model_refs = {model_ref("model_b"), model_ref("model_a")}});
  require(spec.task_type == batch::kVideoBuildTaskType, "task type");
  require(spec.inputs.size() == 2 && spec.inputs.contains(std::string(batch::kVideoBuildSourceInput)) &&
              spec.inputs.contains(std::string(batch::kVideoBuildModelLockInput)),
          "source and model lock inputs");
  require(spec.model_refs.front().model_id == "model_a", "model refs sorted for canonical bytes");
}

// A Mac that answers from a script of outcomes, then builds.
class ScriptedMac final : public batch::RemoteVideoBuilder {
 public:
  ScriptedMac(std::string name, std::vector<batch::RemoteVideoStatus> script)
      : name_(std::move(name)), script_(std::move(script)) {}
  std::string name() const override { return name_; }
  batch::RemoteVideoOutcome build(const batch::RemoteVideoRequest& request) override {
    const batch::RemoteVideoStatus status =
        calls_ < script_.size() ? script_[calls_] : batch::RemoteVideoStatus::built;
    ++calls_;
    if (status == batch::RemoteVideoStatus::built) {
      write_text(request.output_path, "built by " + name_);
      if (!request.run_report_path.empty()) {
        write_text(request.run_report_path, "{}");
      }
    }
    return batch::RemoteVideoOutcome{.status = status, .message = "scripted"};
  }
  std::size_t calls() const { return calls_; }

 private:
  std::string name_;
  std::vector<batch::RemoteVideoStatus> script_;
  std::size_t calls_ = 0;
};

void test_dispatch_requeues_what_other_macs_give_back() {
  constexpr std::size_t kItems = 6;
  std::mutex mutex;
  std::map<std::size_t, std::string> done_by;
  auto busy_once = std::make_shared<ScriptedMac>(
      "busy", std::vector{batch::RemoteVideoStatus::busy});
  auto gone = std::make_shared<ScriptedMac>(
      "gone", std::vector{batch::RemoteVideoStatus::unavailable});
  batch::dispatch_batch(batch::BatchDispatchOptions{
      .item_count = kItems,
      .local_slots = 1,
      .run_local =
          [&](std::size_t index) {
            const std::lock_guard lock(mutex);
            require(!done_by.contains(index), "each item runs once");
            done_by[index] = "local";
          },
      .remote_macs = {busy_once, gone},
      .run_remote =
          [&](std::size_t index, batch::RemoteVideoBuilder& mac) {
            const batch::RemoteVideoOutcome outcome = mac.build(batch::RemoteVideoRequest{
                .item_id = "video_" + std::to_string(index),
                .parameters = {},
                .source_path = {},
                .output_path = std::filesystem::temp_directory_path() / "svp-batch-test-unused",
                .run_report_path = {}});
            if (outcome.status == batch::RemoteVideoStatus::built ||
                outcome.status == batch::RemoteVideoStatus::failed) {
              const std::lock_guard lock(mutex);
              require(!done_by.contains(index), "each item runs once");
              done_by[index] = mac.name();
            }
            return outcome;
          },
      .busy_backoff = std::chrono::milliseconds{1}});
  require(done_by.size() == kItems, "every item was built");
  // It may not get an item at all before the others finish them.
  require(gone->calls() <= 1, "an unavailable Mac takes no second item");
  for (const auto& [index, mac] : done_by) {
    require(mac != "gone", "nothing counts as built by an unavailable Mac");
  }
  std::error_code error;
  std::filesystem::remove(std::filesystem::temp_directory_path() / "svp-batch-test-unused", error);
}

void test_dispatch_alone_runs_every_item_here_in_order() {
  std::vector<std::size_t> order;
  batch::dispatch_batch(batch::BatchDispatchOptions{
      .item_count = 4,
      .local_slots = 1,
      .run_local = [&](std::size_t index) { order.push_back(index); },
      .remote_macs = {},
      .run_remote = {},
      .busy_backoff = std::chrono::milliseconds{0}});
  require(order == (std::vector<std::size_t>{0, 1, 2, 3}), "one slot, batch order");
}

void test_build_batch_outputs() {
  TemporaryDirectory dir("svp-build-batch");
  const std::filesystem::path sources = dir.path / "in";
  const std::filesystem::path out = dir.path / "out";
  std::filesystem::create_directories(sources);
  std::filesystem::create_directories(sources / "other");
  write_text(sources / "a.src", "a");
  write_text(sources / "b.src", "b");
  write_text(sources / "other" / "a.src", "a again");
  std::filesystem::create_directories(out);
  write_text(out / "b.svp", "an earlier output");

  std::atomic<int> local_builds{0};
  batch::BuildBatchOptions options;
  options.sources = {sources / "a.src", sources / "b.src", sources / "other" / "a.src"};
  options.out_dir = out;
  options.parameters = sample_parameters();
  options.parameters.output_format = batch::VideoOutputFormat::svp;
  options.quiet = true;
  options.run_local = [&](const batch::VideoBuildRunOptions& run, const std::filesystem::path&) {
    ++local_builds;
    require(run.parameters.source_name == run.source_path.filename().string(),
            "each video carries its own source name");
    write_text(run.output_path, "built here");
    return batch::VideoBuildRunResult{.success = true};
  };
  const batch::BuildBatchResult refused = batch::build_batch(options);
  require(refused.items[0].status == batch::BuildBatchStatus::created &&
              refused.items[0].artifact_path == out / "a.svp",
          "a new output is built: <stem>.svp");
  require(refused.items[1].status == batch::BuildBatchStatus::failed,
          "an existing output is refused by default");
  require(refused.items[2].status == batch::BuildBatchStatus::failed &&
              refused.items[2].error_message.find("same output") != std::string::npos,
          "two videos writing one output: the second fails");
  require(local_builds == 1, "only the new video was built");

  options.existing = batch::ExistingOutputs::rebuild;
  options.sources = {sources / "a.src", sources / "b.src"};
  auto remote = std::make_shared<ScriptedMac>("mac-2", std::vector<batch::RemoteVideoStatus>{});
  options.coordinators = {remote};
  const batch::BuildBatchResult rebuilt = batch::build_batch(options);
  require(rebuilt.all_succeeded(), "--fresh rebuilds every video");
  std::set<std::string> builders;
  for (const auto& item : rebuilt.items) {
    builders.insert(item.built_on);
  }
  require(remote->calls() + static_cast<std::size_t>(local_builds.load()) == 1 + 2,
          "each video was built once, here or on the other Mac");
  require(!builders.empty(), "every video names the Mac that built it");
  require(rebuilt.items[0].run_report_path == out / "a.svp.run-report.json",
          "the run report sits beside its output");
}

class FakeLoad final : public svp::builder::LocalTaskLoad {
 public:
  void started(std::string_view type) override {
    const std::lock_guard lock(mutex_);
    ++running_[std::string(type)];
  }
  void finished(std::string_view type) override {
    const std::lock_guard lock(mutex_);
    --running_[std::string(type)];
  }
  int running(const std::string& type) {
    const std::lock_guard lock(mutex_);
    return running_[type];
  }

 private:
  std::mutex mutex_;
  std::map<std::string, int> running_;
};

class HeldExecutor final : public exec::Executor {
 public:
  std::string_view id() const override { return "held"; }
  std::size_t slots() const override { return 2; }
  void start(exec::ExecutorEvents& events) override { events_ = &events; }
  void assign(const exec::TaskSpec&, const exec::Lease& lease) override {
    leases_.push_back(lease.lease_id);
  }
  void cancel(std::string_view) override {}
  void lease_expired(std::string_view) override {}
  void stop() override {}
  exec::ExecutorEvents* events_ = nullptr;
  std::vector<std::string> leases_;
};

class IgnoredEvents final : public exec::ExecutorEvents {
 public:
  void lease_heartbeat(std::string_view) override {}
  void attempt_finished(std::string_view, exec::AttemptOutput) override { ++finished; }
  void attempt_failed(std::string_view, exec::AttemptFailureKind, std::string) override {
    ++failed;
  }
  void attempt_rejected(std::string_view, std::string, std::string) override { ++rejected; }
  int finished = 0;
  int failed = 0;
  int rejected = 0;
};

void test_local_tasks_are_reported_while_they_run() {
  auto load = std::make_shared<FakeLoad>();
  HeldExecutor inner;
  IgnoredEvents events;
  engine::LoadReportingExecutor executor(inner, load);
  executor.start(events);
  exec::TaskSpec spec;
  spec.task_type = "ocr.frame_batch";
  executor.assign(spec, exec::Lease{.lease_id = "l1", .attempt = 1});
  executor.assign(spec, exec::Lease{.lease_id = "l2", .attempt = 1});
  require(load->running("ocr.frame_batch") == 2, "both running tasks are reported");
  inner.events_->attempt_finished("l1", exec::AttemptOutput{});
  require(load->running("ocr.frame_batch") == 1 && events.finished == 1,
          "a finished task ends, and the scheduler still hears of it");
  executor.cancel("l2");
  require(load->running("ocr.frame_batch") == 0, "a cancelled lease no longer counts");
  inner.events_->attempt_failed("l2", exec::AttemptFailureKind::executor_lost, "late");
  require(load->running("ocr.frame_batch") == 0 && events.failed == 1,
          "a late report of a cancelled lease is not counted twice");
}

}  // namespace

int main() {
  test_parameters_round_trip_and_are_strict();
  test_package_record_round_trips();
  test_spec_names_inputs_and_models();
  test_dispatch_requeues_what_other_macs_give_back();
  test_dispatch_alone_runs_every_item_here_in_order();
  test_build_batch_outputs();
  test_local_tasks_are_reported_while_they_run();
  std::cout << "batch tests passed\n";
  return 0;
}
