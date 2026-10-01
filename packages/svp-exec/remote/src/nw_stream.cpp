#include "nw_stream.hpp"

#include "svp/exec/exec_error.hpp"
#include "tls_psk_parameters.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace svp::exec::remote::detail {

struct NwStream::Shared {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::function<void()> on_change;
  // Released once the connection reports `cancelled`, which breaks the
  // connection -> handler block -> Shared -> connection cycle.
  NwRef<nw_connection_t> connection;

  StreamPhase phase = StreamPhase::connecting;
  std::chrono::steady_clock::time_point started_at{};
  std::chrono::steady_clock::time_point ready_at{};
  std::string failure;
  bool failed_in_tls = false;
  TlsSession tls;
  std::string peer;

  // Read-ahead buffer: bytes [offset, buffer.size()) are unread.
  std::vector<std::byte> buffer;
  std::size_t offset = 0;
  bool receive_outstanding = false;
  bool end_of_stream = false;
  std::string receive_error;

  [[nodiscard]] std::size_t buffered() const { return buffer.size() - offset; }
};

namespace {

using Shared = NwStream::Shared;

// Errors that mean "the stream is over" rather than "the stream broke": a
// local cancel, or a peer that reset or closed the connection. The worker
// loop and executors treat end of stream as a lost peer either way; this
// only keeps a normal teardown from being reported as a read failure.
bool is_end_of_stream_error(nw_error_t error) {
  if (nw_error_get_error_domain(error) != nw_error_domain_posix) {
    return false;
  }
  const int code = nw_error_get_error_code(error);
  return code == ECANCELED || code == ECONNRESET || code == ENOTCONN || code == EPIPE;
}

std::string describe_remote_endpoint(nw_connection_t connection) {
  nw_path_t path = nw_connection_copy_current_path(connection);
  if (path == nullptr) {
    return {};
  }
  std::string text;
  if (nw_endpoint_t remote = nw_path_copy_effective_remote_endpoint(path)) {
    if (nw_endpoint_get_type(remote) == nw_endpoint_type_address) {
      if (char* address = nw_endpoint_copy_address_string(remote)) {
        text = address;
        std::free(address);
      }
      text += " port " + std::to_string(nw_endpoint_get_port(remote));
    }
    nw_release(remote);
  }
  nw_release(path);
  return text;
}

void ensure_receive(const std::shared_ptr<Shared>& shared);

void on_receive(const std::shared_ptr<Shared>& shared, dispatch_data_t content,
                nw_content_context_t context, bool is_complete, nw_error_t error) {
  {
    const std::lock_guard lock(shared->mutex);
    shared->receive_outstanding = false;
    if (content != nullptr) {
      if (shared->offset == shared->buffer.size()) {
        shared->buffer.clear();
        shared->offset = 0;
      } else if (shared->offset > shared->buffer.size() / 2) {
        shared->buffer.erase(shared->buffer.begin(),
                             shared->buffer.begin() + static_cast<std::ptrdiff_t>(shared->offset));
        shared->offset = 0;
      }
      std::vector<std::byte>* target = &shared->buffer;
      dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* bytes, size_t size) {
        const auto* first = static_cast<const std::byte*>(bytes);
        target->insert(target->end(), first, first + size);
        return true;
      });
    }
    if (is_complete && (context == nullptr || nw_content_context_get_is_final(context))) {
      shared->end_of_stream = true;
    }
    if (error != nullptr) {
      if (is_end_of_stream_error(error)) {
        shared->end_of_stream = true;
      } else {
        shared->receive_error = describe_nw_error(error);
      }
    }
    ensure_receive(shared);
  }
  shared->changed.notify_all();
}

// Caller holds shared->mutex. Keeps one receive outstanding while the
// read-ahead buffer has room.
void ensure_receive(const std::shared_ptr<Shared>& shared) {
  if (shared->receive_outstanding || shared->end_of_stream || !shared->receive_error.empty() ||
      shared->phase != StreamPhase::ready || !shared->connection) {
    return;
  }
  const std::size_t buffered = shared->buffered();
  if (buffered >= kReceiveAheadBytes) {
    return;
  }
  shared->receive_outstanding = true;
  const std::shared_ptr<Shared> self = shared;
  nw_connection_receive(shared->connection.get(), 1,
                        static_cast<std::uint32_t>(kReceiveAheadBytes - buffered),
                        ^(dispatch_data_t content, nw_content_context_t context,
                          bool is_complete, nw_error_t error) {
                          on_receive(self, content, context, is_complete, error);
                        });
}

void on_state(const std::shared_ptr<Shared>& shared, nw_connection_state_t state,
              nw_error_t error) {
  std::function<void()> notify;
  // A connection that fails is cancelled at once. Network framework does not
  // complete a send that is waiting for the peer's window when the
  // connection fails until the connection is cancelled, so a peer that
  // resets while write_all waits (a route probe cancelled mid-reply, a
  // coordinator or worker that goes away) would otherwise block that writer
  // until the owner cancels from elsewhere. Cancelling completes every
  // outstanding send and receive.
  NwRef<nw_connection_t> cancel_failed;
  {
    const std::lock_guard lock(shared->mutex);
    const auto fail = [&](std::string_view prefix) {
      shared->phase = StreamPhase::failed;
      shared->failure = std::string(prefix) + describe_nw_error(error);
      shared->failed_in_tls =
          error != nullptr && nw_error_get_error_domain(error) == nw_error_domain_tls;
      cancel_failed = shared->connection;
    };
    switch (state) {
      case nw_connection_state_waiting:
        // The framework would retry forever; to a caller that pinned an
        // interface this route is simply not usable now.
        if (shared->phase == StreamPhase::connecting) {
          fail("waiting: ");
        }
        break;
      case nw_connection_state_ready:
        if (shared->phase == StreamPhase::connecting && shared->connection) {
          shared->phase = StreamPhase::ready;
          shared->ready_at = std::chrono::steady_clock::now();
          shared->tls = read_tls_session(shared->connection.get());
          shared->peer = describe_remote_endpoint(shared->connection.get());
          ensure_receive(shared);
        }
        break;
      case nw_connection_state_failed:
        if (shared->phase == StreamPhase::connecting || shared->phase == StreamPhase::ready) {
          fail("");
        }
        break;
      case nw_connection_state_cancelled:
        // A failed stream stays failed so readers report why it ended.
        if (shared->phase != StreamPhase::failed) {
          shared->phase = StreamPhase::cancelled;
        }
        shared->connection.reset();
        break;
      default:
        break;
    }
    notify = shared->on_change;
  }
  if (cancel_failed) {
    nw_connection_cancel(cancel_failed.get());
  }
  shared->changed.notify_all();
  if (notify) {
    notify();
  }
}

[[noreturn]] void throw_closed(std::string_view what, std::string_view why) {
  throw ExecError(ExecErrorCode::frame_truncated, std::string(what) + ": " + std::string(why));
}

}  // namespace

NwStream::NwStream(NwRef<nw_connection_t> connection, std::function<void()> on_change)
    : queue_("org.svp.exec.remote.stream"),
      connection_(std::move(connection)),
      shared_(std::make_shared<Shared>()) {
  shared_->on_change = std::move(on_change);
  shared_->connection = connection_;
  const std::shared_ptr<Shared> shared = shared_;
  nw_connection_set_queue(connection_.get(), queue_.get());
  nw_connection_set_state_changed_handler(
      connection_.get(), ^(nw_connection_state_t state, nw_error_t error) {
        on_state(shared, state, error);
      });
}

NwStream::~NwStream() {
  cancel();
  // Handlers still queued may run later; they only touch Shared, which they
  // keep alive. One that already copied on_change may still call it, so
  // owners capture whatever it touches by shared_ptr.
  const std::lock_guard lock(shared_->mutex);
  shared_->on_change = nullptr;
}

void NwStream::start() {
  {
    const std::lock_guard lock(shared_->mutex);
    shared_->started_at = std::chrono::steady_clock::now();
  }
  nw_connection_start(connection_.get());
}

StreamPhase NwStream::phase() const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->phase;
}

std::chrono::microseconds NwStream::handshake_time() const {
  const std::lock_guard lock(shared_->mutex);
  if (shared_->ready_at == std::chrono::steady_clock::time_point{}) {
    return std::chrono::microseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::microseconds>(shared_->ready_at -
                                                               shared_->started_at);
}

std::string NwStream::failure() const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->failure;
}

bool NwStream::failed_in_tls() const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->failed_in_tls;
}

bool NwStream::wait_ready(std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(shared_->mutex);
  shared_->changed.wait_until(lock, deadline,
                              [&] { return shared_->phase != StreamPhase::connecting; });
  return shared_->phase == StreamPhase::ready;
}

std::size_t NwStream::read_some(std::span<std::byte> buffer) {
  if (buffer.empty()) {
    return 0;
  }
  Shared& shared = *shared_;
  std::unique_lock lock(shared.mutex);
  while (true) {
    if (shared.buffered() > 0) {
      const std::size_t count = std::min(buffer.size(), shared.buffered());
      std::copy_n(shared.buffer.begin() + static_cast<std::ptrdiff_t>(shared.offset), count,
                  buffer.begin());
      shared.offset += count;
      ensure_receive(shared_);
      return count;
    }
    if (shared.end_of_stream || shared.phase == StreamPhase::cancelled) {
      return 0;
    }
    if (!shared.receive_error.empty()) {
      throw_closed("frame read failed", shared.receive_error);
    }
    if (shared.phase == StreamPhase::failed && !shared.receive_outstanding) {
      throw_closed("frame read failed", shared.failure);
    }
    ensure_receive(shared_);
    shared.changed.wait(lock);
  }
}

void NwStream::write_all(std::span<const std::byte> bytes) {
  NwRef<nw_connection_t> connection;
  {
    const std::lock_guard lock(shared_->mutex);
    if (shared_->phase != StreamPhase::ready || !shared_->connection) {
      throw_closed("frame write failed",
                   shared_->failure.empty() ? "connection is not open" : shared_->failure);
    }
    connection = shared_->connection;
  }
  if (bytes.empty()) {
    return;
  }
  struct SendWait {
    std::mutex mutex;
    std::condition_variable done_changed;
    bool done = false;
    std::string error;
  };
  const auto wait = std::make_shared<SendWait>();
  dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr,
                                              DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  // Each write is one complete message on the stream: the framework sends
  // it now instead of holding it for more content in the same context. It
  // does not close the connection (that is the final context's job).
  nw_connection_send(connection.get(), data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true,
                     ^(nw_error_t error) {
                       {
                         const std::lock_guard lock(wait->mutex);
                         wait->done = true;
                         if (error != nullptr) {
                           wait->error = describe_nw_error(error);
                         }
                       }
                       wait->done_changed.notify_all();
                     });
  dispatch_release(data);
  std::unique_lock lock(wait->mutex);
  wait->done_changed.wait(lock, [&] { return wait->done; });
  if (!wait->error.empty()) {
    // A send cut off by cancelling a failed connection reports the failure,
    // not the cancel.
    std::string failure;
    {
      const std::lock_guard shared_lock(shared_->mutex);
      failure = shared_->failure;
    }
    throw_closed("frame write failed", failure.empty() ? wait->error : failure);
  }
}

void NwStream::cancel() { nw_connection_cancel(connection_.get()); }

TlsSession NwStream::tls_session() const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->tls;
}

std::string NwStream::peer_description() const {
  const std::lock_guard lock(shared_->mutex);
  return shared_->peer;
}

}  // namespace svp::exec::remote::detail
