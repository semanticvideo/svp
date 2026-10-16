#include "paired_piece_encoders.hpp"

#include "coordinator_context.hpp"
#include "silence_watch.hpp"
#include "worker_restart.hpp"
#include "worker_supplies.hpp"

#include "resize/piece_task.hpp"

#include "svp/exec/lease_frames.hpp"
#include "svp/exec/lease_policy.hpp"
#include "svp/exec/task_frames.hpp"
#include "svp/exec/worker/blob_source.hpp"
#include "svp/exec/worker/coordinator_session.hpp"
#include "svp/exec/worker/pairing_store.hpp"
#include "svp/exec/worker/worker_connection.hpp"
#include "svp/vision/tasks/ffmpeg_build_identity.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace svp::builder::workers {
namespace {

using namespace svp::exec::worker;
using svp::builder::resize::PieceJob;
using svp::builder::resize::PieceQueue;

std::string resize_session_id() {
  return "resize_" + std::to_string(static_cast<long long>(::getpid())) + "_" +
         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

std::string piece_task_id(std::size_t index) { return "piece_" + std::to_string(index); }

std::string mac_name(const CoordinatorPairingRecord& record) {
  return record.key.pairing_id + " (" +
         (record.worker.ssh_target.empty() ? record.worker.join_id : record.worker.ssh_target) +
         ")";
}

struct Shared {
  PieceQueue& queue;
  svp::builder::resize::ResizePieceParameters parameters;
  std::shared_ptr<const WorkerSupplies> supplies;
  std::string session_id;
  const std::function<void(const std::string&)>& report;
  const svp::exec::CancellationToken* cancellation;
  std::mutex report_mutex;

  bool stopping() const {
    return (cancellation != nullptr && cancellation->requested()) || queue.closed();
  }
  void tell(const std::string& line) {
    if (report) {
      const std::lock_guard lock(report_mutex);
      report(line);
    }
  }
};

enum class PieceEnd { encoded, stop };

// One piece on an open session. Gives the piece back unless it was encoded.
PieceEnd encode_on(Shared& shared, WorkerConnection& connection, WorkerSessionClient& client,
                   const WorkerHelloAck& ack, const PieceJob& job, const std::string& mac,
                   std::uint64_t& leases) {
  const BlobSource piece = describe_blob_file(job.piece);
  TransferStats stats;
  client.send_blobs({piece}, stats);
  const svp::exec::TaskSpec spec = svp::builder::resize::make_resize_piece_spec(
      shared.session_id, piece_task_id(job.index),
      svp::exec::ArtifactRef{.blake3 = piece.ref.blake3, .bytes = piece.ref.bytes},
      shared.parameters);
  const svp::exec::Lease lease{.lease_id = "lease." + spec.task_id + "." + std::to_string(++leases),
                               .attempt = 1,
                               .duration = svp::exec::kDefaultLeaseFloor,
                               .heartbeat_interval = svp::exec::kDefaultHeartbeatInterval};
  std::optional<svp::exec::Frame> answer;
  bool silent = false;
  bool stopped = false;
  {
    SilenceWatch watch(*connection.connection.stream, lease.duration,
                       [&shared] { return shared.stopping(); });
    connection.writer->write(svp::exec::make_leased_assign_frame(spec, lease));
    while (true) {
      std::optional<svp::exec::Frame> frame = connection.reader->read();
      if (!frame) {
        silent = watch.expired();
        stopped = watch.stopped_by_request();
        break;
      }
      watch.heard();
      if (frame->type == svp::exec::MessageType::heartbeat) {
        continue;
      }
      answer = std::move(frame);
      break;
    }
  }
  const auto give_back = [&](const std::string& why) {
    shared.queue.give_back(job.index);
    if (!why.empty()) {
      shared.tell("resize: " + mac + " stopped taking pieces: " + why);
    }
    return PieceEnd::stop;
  };
  if (!answer) {
    return give_back(stopped ? std::string()
                             : silent ? "it stopped answering" : "it closed the connection");
  }
  if (answer->type != svp::exec::MessageType::result) {
    return give_back("it answered " + std::string(svp::exec::message_type_name(answer->type)));
  }
  const svp::exec::TaskResult result = svp::exec::task_result_from_result_frame(*answer);
  const auto release = [&](const std::vector<BlobRef>& blobs) {
    if (worker_accepts_blob_release(ack.protocol)) {
      client.release_blobs(blobs);
    }
  };
  if (result.status == svp::exec::TaskStatus::failed) {
    release({piece.ref});
    return give_back(result.error ? result.error->message : "it failed the piece");
  }
  std::optional<svp::builder::resize::EncodedPieceRecord> record;
  for (std::size_t index = 0; index < result.outputs.size(); ++index) {
    if (result.outputs[index].role == svp::builder::resize::kResizePieceEncodedRole) {
      const std::vector<std::byte>& payload = answer->payloads.at(index);
      record = svp::builder::resize::decode_encoded_piece_record(
          std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    }
  }
  if (!record) {
    release({piece.ref});
    return give_back("its result names no encoded piece");
  }
  const BlobRef encoded{.blake3 = record->blake3, .bytes = record->bytes};
  client.fetch_blob(encoded, job.encoded);
  shared.queue.encoded(job.index);
  release({piece.ref, encoded});
  return PieceEnd::encoded;
}

// Every piece this Mac takes until the queue has none left or it stops.
void serve(Shared& shared, const CoordinatorPairingRecord& record) {
  const std::string mac = mac_name(record);
  const std::shared_ptr<RuntimeSwitchWatch> switch_watch =
      runtime_switch_watch(record.key.pairing_id, shared.supplies->runtime);
  std::optional<PieceJob> job;
  try {
    // An earlier session left this Mac switching to another runtime: wait
    // for it to come back on it.
    if (const std::optional<WorkerHelloAck> due = switch_watch->take_due()) {
      switch_watch->after_wait(
          await_worker_runtime_switch(record.key, shared.supplies->hello,
                                      shared.supplies->runtime, *due,
                                      [&shared] { return shared.stopping(); }, {}),
          *due);
    }
    if (shared.stopping()) {
      return;
    }
    std::unique_ptr<WorkerConnection> connection = connect_to_worker(record.key);
    const WorkerHelloAck ack =
        supply_worker_session(*connection->reader, *connection->writer, *shared.supplies).ack;
    switch_watch->observed(ack);
    WorkerSessionClient client(*connection->reader, *connection->writer);
    std::uint64_t leases = 0;
    std::size_t encoded = 0;
    while (!shared.stopping() && (job = shared.queue.take())) {
      const PieceEnd end = encode_on(shared, *connection, client, ack, *job, mac, leases);
      job.reset();
      if (end == PieceEnd::stop) {
        return;
      }
      ++encoded;
    }
    if (job) {
      shared.queue.give_back(job->index);
      job.reset();
    }
    client.shutdown();
    if (encoded > 0) {
      shared.tell("resize: " + mac + " encoded " + std::to_string(encoded) + " pieces");
    }
  } catch (const std::exception& error) {
    if (job) {
      shared.queue.give_back(job->index);
    }
    if (!shared.stopping()) {
      shared.tell("resize: " + mac + " is not taking pieces: " + error.what());
    }
  }
}

}  // namespace

PairedPieceEncoders::PairedPieceEncoders(std::filesystem::path ffmpeg_path)
    : ffmpeg_path_(std::move(ffmpeg_path)) {}

void PairedPieceEncoders::run(PieceQueue& queue,
                              const svp::builder::resize::PieceEncoding& encoding,
                              const std::function<void(const std::string&)>& report,
                              const svp::exec::CancellationToken* cancellation) {
  std::vector<CoordinatorPairingRecord> macs;
  auto supplies = std::make_shared<WorkerSupplies>();
  std::optional<std::string> ffmpeg_build;
  try {
    macs = load_coordinator_pairings(PairingDirectory(default_coordinator_pairings_dir()));
    if (macs.empty()) {
      return;
    }
    ffmpeg_build = svp::vision::tasks::cached_ffmpeg_build_identity(ffmpeg_path_.string());
    const CoordinatorContext context = load_coordinator_context();
    supplies->runtime = context.runtime;
    supplies->hello = context.hello();
    supplies->hello.capacity = {{std::string(svp::builder::resize::kResizePieceTaskType),
                                 svp::builder::resize::kResizePieceSlotsPerMac}};
  } catch (const std::exception& error) {
    if (report) {
      report(std::string("resize: no paired worker takes pieces: ") + error.what());
    }
    return;
  }
  if (!ffmpeg_build) {
    if (report) {
      report("resize: no paired worker takes pieces: this Mac's ffmpeg build cannot be read");
    }
    return;
  }
  Shared shared{.queue = queue,
                .parameters = {.encoding = encoding, .ffmpeg_build = *ffmpeg_build},
                .supplies = std::move(supplies),
                .session_id = resize_session_id(),
                .report = report,
                .cancellation = cancellation};
  std::vector<std::thread> threads;
  for (const CoordinatorPairingRecord& record : macs) {
    threads.emplace_back([&shared, &record] { serve(shared, record); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

}  // namespace svp::builder::workers
