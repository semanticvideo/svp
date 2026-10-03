#include "paired_video_builder.hpp"

#include "../workers/coordinator_context.hpp"
#include "../workers/worker_reach.hpp"
#include "../workers/worker_restart.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/lease_policy.hpp"
#include "svp/exec/task_frames.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/model_bundles.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/exec/worker/worker_error.hpp"
#include "svp/models/hash.hpp"
#include "svp/models/model_lock.hpp"
#include "svp/vision/tasks/model_refs.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace svp::builder::batch {
namespace {

using namespace svp::exec::worker;

constexpr std::string_view kModelLockFileName = "model-lock.json";

// The source's BLAKE3, hashed once per file version for the whole batch:
// a busy Mac asks again every backoff, and a video may go to several Macs.
BlobRef source_blob(const std::filesystem::path& source) {
  struct Entry {
    std::uintmax_t bytes = 0;
    std::filesystem::file_time_type modified;
    BlobRef blob;
  };
  static std::mutex mutex;
  static std::map<std::string, Entry> digests;
  const std::uintmax_t bytes = std::filesystem::file_size(source);
  const std::filesystem::file_time_type modified = std::filesystem::last_write_time(source);
  {
    const std::lock_guard lock(mutex);
    const auto found = digests.find(source.string());
    if (found != digests.end() && found->second.bytes == bytes &&
        found->second.modified == modified) {
      return found->second.blob;
    }
  }
  const std::optional<svp::exec::Blake3Digest> digest =
      svp::exec::parse_blake3_hex(svp::models::blake3_hex_for_file(source));
  if (!digest) {
    throw std::runtime_error("cannot hash " + source.string());
  }
  const BlobRef blob{.blake3 = *digest, .bytes = bytes};
  const std::lock_guard lock(mutex);
  digests[source.string()] = Entry{.bytes = bytes, .modified = modified, .blob = blob};
  return blob;
}

// Ends the connection when the worker has sent nothing for a lease period:
// the job's session heartbeats far more often than that while it builds.
class SilenceWatch {
 public:
  SilenceWatch(svp::exec::remote::RemoteStream& stream, std::chrono::milliseconds limit)
      : stream_(stream), limit_(limit), last_(std::chrono::steady_clock::now()) {
    thread_ = std::thread([this] { watch(); });
  }
  ~SilenceWatch() {
    {
      const std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }
  void heard() {
    const std::lock_guard lock(mutex_);
    last_ = std::chrono::steady_clock::now();
  }
  [[nodiscard]] bool expired() const {
    const std::lock_guard lock(mutex_);
    return expired_;
  }

 private:
  void watch() {
    std::unique_lock lock(mutex_);
    while (!stopped_) {
      const auto deadline = last_ + limit_;
      if (changed_.wait_until(lock, deadline, [&] { return stopped_; })) {
        return;
      }
      if (std::chrono::steady_clock::now() >= last_ + limit_) {
        expired_ = true;
        stream_.cancel();
        return;
      }
    }
  }

  svp::exec::remote::RemoteStream& stream_;
  std::chrono::milliseconds limit_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::chrono::steady_clock::time_point last_;
  bool stopped_ = false;
  bool expired_ = false;
  std::thread thread_;
};

RemoteVideoOutcome unavailable(std::string message) {
  return RemoteVideoOutcome{.status = RemoteVideoStatus::unavailable,
                            .message = std::move(message)};
}

void write_atomically(const std::filesystem::path& destination,
                      const std::vector<std::byte>& bytes) {
  const std::filesystem::path partial = destination.string() + ".partial";
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      throw std::runtime_error("cannot write " + partial.string());
    }
  }
  std::filesystem::rename(partial, destination);
}

std::string batch_session_id() {
  return "batch_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

}  // namespace

std::shared_ptr<const VideoBuildSupplies> prepare_video_build_supplies(
    const std::filesystem::path& model_cache, const svp::models::ThreadPlan& thread_plan) {
  auto supplies = std::make_shared<VideoBuildSupplies>();
  auto base = std::make_shared<svp::builder::workers::WorkerSupplies>();
  base->runtime = locate_coordinator_runtime(svp::builder::workers::current_executable());
  base->hello =
      make_coordinator_hello(base->runtime, thread_plan, model_set_summary(model_cache));
  base->hello.capacity = {{std::string(kVideoBuildTaskType), kVideoBuildSlotsPerMac}};
  const std::filesystem::path lock_path = model_cache / kModelLockFileName;
  const svp::models::ModelLock lock = svp::models::load_model_lock(lock_path);
  std::vector<std::string> model_ids;
  for (const svp::models::ModelLockEntry& entry : lock.models) {
    model_ids.push_back(entry.model_id);
  }
  base->models = prepare_model_bundles(model_cache, model_ids);
  for (const svp::models::ModelLockEntry& entry : lock.models) {
    supplies->model_refs.push_back(svp::vision::tasks::cached_model_ref(model_cache, entry.model_id));
  }
  const std::optional<svp::exec::Blake3Digest> lock_digest =
      svp::exec::parse_blake3_hex(svp::models::blake3_hex_for_file(lock_path));
  supplies->model_lock = BlobSource{
      .ref = BlobRef{.blake3 = *lock_digest, .bytes = std::filesystem::file_size(lock_path)},
      .file = lock_path};
  supplies->base = std::move(base);
  supplies->build_session_id = batch_session_id();
  return supplies;
}

PairedVideoBuilder::PairedVideoBuilder(CoordinatorPairingRecord record,
                                       std::shared_ptr<const VideoBuildSupplies> supplies)
    : record_(std::move(record)),
      supplies_(std::move(supplies)),
      switch_watch_(svp::builder::workers::runtime_offer(supplies_->base->runtime)) {}

std::string PairedVideoBuilder::name() const {
  return record_.key.pairing_id + " (" +
         (record_.worker.ssh_target.empty() ? record_.worker.join_id : record_.worker.ssh_target) +
         ")";
}

RemoteVideoOutcome PairedVideoBuilder::build(const RemoteVideoRequest& request) {
  try {
    const BlobRef source = source_blob(request.source_path);
    const svp::exec::TaskSpec spec = make_video_build_spec(VideoBuildSpecInput{
        .build_session_id = supplies_->build_session_id,
        .task_id = request.item_id,
        .parameters = request.parameters,
        .source = svp::exec::ArtifactRef{.blake3 = source.blake3, .bytes = source.bytes},
        .model_lock = svp::exec::ArtifactRef{.blake3 = supplies_->model_lock.ref.blake3,
                                             .bytes = supplies_->model_lock.ref.bytes},
        .model_refs = supplies_->model_refs});
    svp::builder::workers::WorkerSupplies supplies = *supplies_->base;
    supplies.blobs = {supplies_->model_lock,
                      BlobSource{.ref = source, .file = request.source_path}};

    // The previous job's session left a runtime this Mac's service moves to:
    // it restarts once that session ended, so wait for it to come back.
    if (const std::optional<WorkerHelloAck> due = switch_watch_.take_due()) {
      switch_watch_.after_wait(svp::builder::workers::await_worker_runtime_switch(
                                   record_.key, supplies.hello, supplies.runtime, *due, {}, {}),
                               *due);
    }
    std::unique_ptr<WorkerConnection> connection;
    try {
      connection = connect_to_worker(record_.key);
    } catch (const std::exception& error) {
      return unavailable(std::string("cannot reach it: ") + error.what());
    }
    WorkerHelloAck ack;
    try {
      ack = svp::builder::workers::supply_worker_session(*connection->reader,
                                                         *connection->writer, supplies)
                .ack;
    } catch (const std::exception& error) {
      return unavailable(std::string("cannot prepare it: ") + error.what());
    }
    switch_watch_.observed(ack);

    const svp::exec::Lease lease{
        .lease_id = "lease." + request.item_id + "." + std::to_string(++attempts_),
        .attempt = 1,
        .duration = svp::exec::kDefaultLeaseFloor,
        .heartbeat_interval = svp::exec::kDefaultHeartbeatInterval};
    std::optional<svp::exec::Frame> answer;
    bool silent = false;
    try {
      SilenceWatch watch(*connection->connection.stream, lease.duration);
      connection->writer->write(svp::exec::make_leased_assign_frame(spec, lease));
      while (true) {
        std::optional<svp::exec::Frame> frame = connection->reader->read();
        if (!frame) {
          silent = watch.expired();
          break;
        }
        watch.heard();
        if (frame->type == svp::exec::MessageType::heartbeat) {
          continue;
        }
        answer = std::move(frame);
        break;
      }
    } catch (const std::exception& error) {
      return unavailable(std::string("the connection failed while it built: ") + error.what());
    }
    if (!answer) {
      return unavailable(silent ? "it stopped answering while it built"
                                : "it closed the connection while it built");
    }
    if (answer->type == svp::exec::MessageType::reject) {
      const svp::exec::LeaseRejection rejection =
          svp::exec::lease_rejection_from_frame(*answer);
      return RemoteVideoOutcome{.status = RemoteVideoStatus::busy,
                                .message = rejection.code + ": " + rejection.message};
    }
    if (answer->type != svp::exec::MessageType::result) {
      return unavailable("it answered " +
                         std::string(svp::exec::message_type_name(answer->type)) +
                         (answer->type == svp::exec::MessageType::error ? ": " + answer->body.dump()
                                                                        : std::string()));
    }
    // Once the job has ended on that Mac for good, what it was sent or made
    // for the job (the source, and the package once fetched) is released
    // there (worker protocol 1.2): it deletes them when the job's session
    // ends, unless another coordinator still claims them. The model lock is
    // shared by every job and stays. A Mac that turned the job away, or may
    // be asked again, keeps everything.
    WorkerSessionClient client(*connection->reader, *connection->writer);
    const auto release = [&](const std::vector<BlobRef>& blobs) {
      if (worker_accepts_blob_release(ack.protocol)) {
        client.release_blobs(blobs);
      }
    };
    const svp::exec::TaskResult result = svp::exec::task_result_from_result_frame(*answer);
    if (result.status == svp::exec::TaskStatus::failed) {
      const std::string message = result.error ? result.error->message : "failed";
      if (result.error && result.error->retryable) {
        return unavailable(message);
      }
      release({source});
      client.shutdown();
      return RemoteVideoOutcome{.status = RemoteVideoStatus::failed, .message = message};
    }
    std::optional<VideoBuildPackageRecord> record;
    for (std::size_t index = 0; index < result.outputs.size(); ++index) {
      const std::vector<std::byte>& payload = answer->payloads.at(index);
      if (result.outputs[index].role == kVideoBuildPackageRole) {
        record = decode_video_build_package_record(std::string_view(
            reinterpret_cast<const char*>(payload.data()), payload.size()));
      } else if (result.outputs[index].role == kVideoBuildRunReportRole &&
                 !request.run_report_path.empty()) {
        write_atomically(request.run_report_path, payload);
      }
    }
    if (!record) {
      return unavailable("its result names no package");
    }
    const BlobRef package{.blake3 = record->blake3, .bytes = record->bytes};
    client.fetch_blob(package, request.output_path);
    release({source, package});
    client.shutdown();
    return RemoteVideoOutcome{.status = RemoteVideoStatus::built, .message = {}};
  } catch (const std::exception& error) {
    return unavailable(error.what());
  }
}

std::vector<std::shared_ptr<RemoteVideoBuilder>> paired_video_builders(
    const std::vector<std::string>& coordinators,
    const std::shared_ptr<const VideoBuildSupplies>& supplies) {
  const PairingDirectory pairings(default_coordinator_pairings_dir());
  std::vector<std::shared_ptr<RemoteVideoBuilder>> builders;
  std::vector<std::string> seen;
  for (const std::string& coordinator : coordinators) {
    CoordinatorPairingRecord record =
        svp::builder::workers::find_pairing(pairings, coordinator);
    if (std::find(seen.begin(), seen.end(), record.key.pairing_id) != seen.end()) {
      continue;
    }
    seen.push_back(record.key.pairing_id);
    builders.push_back(std::make_shared<PairedVideoBuilder>(std::move(record), supplies));
  }
  return builders;
}

}  // namespace svp::builder::batch
