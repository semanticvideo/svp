#include "svp/exec/worker/agent_session.hpp"

#include "svp/exec/exec_error.hpp"
#include "svp/exec/fd_frame_io.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/task_frames.hpp"
#include "svp/exec/worker/blob_receiver.hpp"
#include "svp/exec/worker/transfer_messages.hpp"
#include "svp/exec/worker/worker_error.hpp"

#include <condition_variable>
#include <csignal>
#include <map>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace svp::exec::worker {
namespace {

constexpr std::uint64_t kBytesPerMiB = 1024ULL * 1024ULL;

Frame error_frame(std::string_view code, std::string_view message) {
  return Frame{.type = MessageType::error,
               .body = nlohmann::json{{"code", std::string(code)},
                                      {"message", std::string(message)}},
               .payloads = {}};
}

class AgentSession {
 public:
  AgentSession(AgentCore& core, FrameWriter& output, std::string session_id,
               std::function<void()> close_input)
      : core_(core),
        output_(output),
        session_id_(std::move(session_id)),
        close_input_(std::move(close_input)),
        scratch_(core.layout().sessions() / session_id_),
        cas_(core.cas()) {}

  ~AgentSession() { finish(); }

  AgentSession(const AgentSession&) = delete;
  AgentSession& operator=(const AgentSession&) = delete;

  AgentSessionEnd run(FrameReader& input) {
    std::optional<Frame> first;
    try {
      first = input.read();
    } catch (const ExecError& error) {
      send_error("protocol_error", error.what());
      return AgentSessionEnd::protocol_error;
    }
    if (!first) {
      return AgentSessionEnd::input_closed;
    }
    if (first->type != MessageType::hello) {
      send_error("protocol_error", "a session starts with HELLO, got " +
                                       std::string(message_type_name(first->type)));
      return AgentSessionEnd::protocol_error;
    }
    try {
      hello_ = hello_from_frame(*first);
    } catch (const ExecError& error) {
      send_error("protocol_error", error.what());
      return AgentSessionEnd::protocol_error;
    }
    const std::optional<SessionRefusal> refusal = evaluate_hello(hello_, core_.options().host);
    output_.write(make_hello_ack_frame(core_.describe(hello_.runtime_id, refusal)));
    if (refusal) {
      return AgentSessionEnd::refused;
    }

    while (true) {
      std::optional<Frame> frame;
      try {
        frame = input.read();
      } catch (const ExecError& error) {
        send_error("protocol_error", error.what());
        return AgentSessionEnd::protocol_error;
      }
      if (!frame) {
        return lost() ? AgentSessionEnd::session_process_lost : AgentSessionEnd::input_closed;
      }
      try {
        if (frame->type == MessageType::shutdown) {
          forward_shutdown();
          return AgentSessionEnd::shutdown;
        }
        handle(*frame);
      } catch (const WorkerError& error) {
        send_error(worker_error_code_name(error.code()), error.what());
        return AgentSessionEnd::protocol_error;
      } catch (const ExecError& error) {
        send_error("protocol_error", error.what());
        return AgentSessionEnd::protocol_error;
      }
    }
  }

  void finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    {
      const std::lock_guard lock(mutex_);
      ending_ = true;
    }
    if (process_) {
      ::shutdown(process_->fd, SHUT_WR);
      std::unique_lock lock(mutex_);
      if (!relay_done_.wait_for(lock, core_.options().shutdown_grace,
                                [&] { return relay_finished_; })) {
        ::kill(process_->pid, SIGKILL);
      }
      lock.unlock();
      if (relay_.joinable()) {
        relay_.join();
      }
      (void)finish_session_process(*process_, std::chrono::milliseconds{0});
      process_.reset();
    }
    core_.ledger().release_session(session_id_);
    receiver_.reset();
    std::error_code error;
    std::filesystem::remove_all(scratch_, error);
  }

 private:
  void handle(const Frame& frame) {
    switch (frame.type) {
      case MessageType::runtime_have: {
        const Blake3Digest runtime_id = runtime_query_from_frame(frame);
        output_.write(make_runtime_answer_frame(
            RuntimeAnswer{.runtime_id = runtime_id, .present = core_.runtimes().has(runtime_id)}));
        return;
      }
      case MessageType::runtime_put: {
        const RuntimePut put = runtime_put_from_frame(frame);
        core_.runtimes().install_from_cas(put.runtime_id, put.manifest, put.components, cas_);
        return;
      }
      case MessageType::blob_have: {
        const BlobHaveQuery query = blob_have_query_from_frame(frame);
        if (const auto* blobs = std::get_if<BlobQuery>(&query)) {
          BlobQuery missing;
          for (const BlobRef& blob : blobs->blobs) {
            if (!cas_.has(blob.blake3)) {
              missing.blobs.push_back(blob);
            }
          }
          output_.write(make_blob_answer_frame(missing));
        } else {
          ModelBundleQuery missing;
          for (const Blake3Digest& bundle : std::get<ModelBundleQuery>(query).bundles) {
            if (!core_.models().has(bundle)) {
              missing.bundles.push_back(bundle);
            }
          }
          output_.write(make_model_bundle_answer_frame(missing));
        }
        return;
      }
      case MessageType::blob_put: {
        const BlobPut put = blob_put_from_frame(frame);
        if (const auto* chunk = std::get_if<BlobChunk>(&put)) {
          receiver().put_chunk(*chunk, frame.payloads.front());
        } else {
          const ModelBundlePut& bundle = std::get<ModelBundlePut>(put);
          core_.models().install_from_cas(bundle.lock, bundle.manifest, cas_);
        }
        return;
      }
      case MessageType::assign:
        assign(frame);
        return;
      case MessageType::cancel: {
        const std::string lease_id = lease_id_from_cancel_frame(frame);
        release(lease_id);
        if (child_writer_) {
          write_to_child(frame);
        }
        return;
      }
      default:
        throw WorkerError(WorkerErrorCode::protocol,
                          "unexpected " + std::string(message_type_name(frame.type)) +
                              " frame from the coordinator");
    }
  }

  BlobReceiver& receiver() {
    if (!receiver_) {
      receiver_ = std::make_unique<BlobReceiver>(scratch_ / "incoming", cas_);
    }
    return *receiver_;
  }

  void assign(const Frame& frame) {
    const LeasedAssignment assignment = leased_assignment_from_frame(frame);
    ensure_session_process();
    const std::uint64_t required = assignment.spec.resources.est_peak_rss_mb * kBytesPerMiB;
    const AdmissionDecision decision = core_.ledger().try_admit(
        session_id_, assignment.lease.lease_id, required, core_.memory());
    if (!decision.admitted) {
      output_.write(make_reject_frame(LeaseRejection{.lease_id = assignment.lease.lease_id,
                                                     .code = decision.code,
                                                     .message = decision.message}));
      return;
    }
    {
      const std::lock_guard lock(mutex_);
      leases_[{assignment.spec.task_id, assignment.lease.attempt}] = assignment.lease.lease_id;
    }
    write_to_child(frame);
  }

  void release(const std::string& lease_id) {
    core_.ledger().release(session_id_, lease_id);
    const std::lock_guard lock(mutex_);
    std::erase_if(leases_, [&](const auto& entry) { return entry.second == lease_id; });
  }

  void write_to_child(const Frame& frame) {
    try {
      child_writer_->write(frame);
    } catch (const ExecError&) {
      // The session process is gone; the relay notices and ends the session.
    }
  }

  void forward_shutdown() {
    {
      const std::lock_guard lock(mutex_);
      ending_ = true;
    }
    if (child_writer_) {
      write_to_child(make_shutdown_frame());
    }
  }

  void ensure_session_process() {
    if (process_) {
      return;
    }
    if (!core_.runtimes().has(hello_.runtime_id)) {
      throw WorkerError(WorkerErrorCode::protocol,
                        "runtime " + blake3_prefixed(hello_.runtime_id) +
                            " is not installed on this worker; send it with RUNTIME_PUT first");
    }
    // Plan §3.2: verified by BLAKE3 before execution, every session.
    core_.runtimes().verify(hello_.runtime_id);
    std::error_code error;
    std::filesystem::create_directories(scratch_, error);
    if (error) {
      throw WorkerError(WorkerErrorCode::io, "cannot create " + scratch_.string());
    }
    process_ = core_.launch(make_session_launch(core_.runtimes().directory_of(hello_.runtime_id),
                                                core_.layout().cas(), scratch_,
                                                core_.layout().models(), session_id_,
                                                hello_.runtime_id));
    child_writer_ = std::make_unique<FdFrameWriter>(process_->fd, core_.options().frame_limits);
    relay_ = std::thread([this, fd = process_->fd] { relay(fd); });
  }

  void relay(int fd) {
    FdFrameReader reader(fd, core_.options().frame_limits);
    try {
      while (std::optional<Frame> frame = reader.read()) {
        if (frame->type == MessageType::result) {
          const TaskResult result = task_result_from_result_frame(*frame);
          std::string lease_id;
          {
            const std::lock_guard lock(mutex_);
            const auto found = leases_.find({result.task_id, result.attempt});
            if (found != leases_.end()) {
              lease_id = found->second;
              leases_.erase(found);
            }
          }
          if (!lease_id.empty()) {
            core_.ledger().release(session_id_, lease_id);
          }
        }
        output_.write(*frame);
      }
    } catch (const ExecError&) {
      // Bad bytes from the session process, or the coordinator is gone.
    }
    bool ending = false;
    {
      const std::lock_guard lock(mutex_);
      relay_finished_ = true;
      ending = ending_;
      lost_ = !ending;
      relay_done_.notify_all();
    }
    if (!ending && close_input_) {
      close_input_();
    }
  }

  bool lost() {
    const std::lock_guard lock(mutex_);
    return lost_;
  }

  void send_error(std::string_view code, std::string_view message) {
    try {
      output_.write(error_frame(code, message));
    } catch (const ExecError&) {
      // The coordinator is gone; nothing to report to.
    }
  }

  AgentCore& core_;
  FrameWriter& output_;
  std::string session_id_;
  std::function<void()> close_input_;
  std::filesystem::path scratch_;
  CasStore cas_;
  CoordinatorHello hello_;
  std::unique_ptr<BlobReceiver> receiver_;
  std::optional<SessionProcess> process_;
  std::unique_ptr<FdFrameWriter> child_writer_;
  std::thread relay_;
  std::mutex mutex_;
  std::condition_variable relay_done_;
  bool relay_finished_ = false;
  bool ending_ = false;
  bool lost_ = false;
  bool finished_ = false;
  // (task_id, attempt) -> lease_id of admitted leases.
  std::map<std::pair<std::string, std::uint64_t>, std::string> leases_;
};

}  // namespace

AgentCore::AgentCore(AgentCoreOptions options)
    : options_(std::move(options)),
      ledger_(options_.admission, options_.host.physical_memory_bytes),
      runtimes_(options_.layout.runtimes()),
      models_(options_.layout.models()),
      cas_([&] {
        create_worker_layout(options_.layout);
        CacheResult<CasStore> store = CasStore::at(options_.layout.cas());
        if (!store) {
          throw WorkerError(WorkerErrorCode::io, "cannot open the worker cache " +
                                                     options_.layout.cas().string() + ": " +
                                                     store.error().message);
        }
        return std::move(store).value();
      }()) {
  if (!options_.sample_memory) {
    options_.sample_memory = [] { return sample_memory(); };
  }
  if (!options_.launcher) {
    options_.launcher = [](const SessionLaunch& launch) { return spawn_session_process(launch); };
  }
}

MemorySnapshot AgentCore::memory() const { return options_.sample_memory(); }

SessionProcess AgentCore::launch(const SessionLaunch& launch) const {
  return options_.launcher(launch);
}

WorkerHelloAck AgentCore::describe(const Blake3Digest& requested_runtime,
                                   std::optional<SessionRefusal> refusal) const {
  WorkerHelloAck ack;
  ack.refusal = std::move(refusal);
  ack.host = options_.host;
  ack.memory = memory();
  ack.memory_reserve_bytes = ledger_.reserve_bytes();
  try {
    ack.disk_available_bytes = available_disk_bytes(options_.layout.root);
  } catch (const WorkerError&) {
    ack.disk_available_bytes = 0;
  }
  if (CacheResult<CacheUsage> usage = cas_.usage()) {
    ack.cache_blob_count = usage.value().blob_count;
    ack.cache_total_bytes = usage.value().total_bytes;
  }
  ack.runtime_present = runtimes_.has(requested_runtime);
  ack.runtimes = runtimes_.list();
  ack.model_bundles = models_.list();
  ack.active_sessions = active_sessions.load();
  ack.agent_runtime_id = options_.agent_runtime_id;
  return ack;
}

std::string_view agent_session_end_name(AgentSessionEnd end) noexcept {
  switch (end) {
    case AgentSessionEnd::refused:
      return "refused";
    case AgentSessionEnd::input_closed:
      return "input_closed";
    case AgentSessionEnd::shutdown:
      return "shutdown";
    case AgentSessionEnd::protocol_error:
      return "protocol_error";
    case AgentSessionEnd::session_process_lost:
      return "session_process_lost";
  }
  return "unknown";
}

AgentSessionEnd serve_agent_session(AgentCore& core, FrameReader& input, FrameWriter& output,
                                    const std::string& worker_session_id,
                                    const std::function<void()>& close_input) {
  ++core.active_sessions;
  AgentSessionEnd end = AgentSessionEnd::input_closed;
  {
    AgentSession session(core, output, worker_session_id, close_input);
    end = session.run(input);
    session.finish();
  }
  --core.active_sessions;
  return end;
}

}  // namespace svp::exec::worker
