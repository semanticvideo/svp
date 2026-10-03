// StreamDeadline: a session that outlasts its limit has its stream
// cancelled, so a read blocked on a silent peer ends; a session that ends
// first leaves the stream alone.

#include "workers/stream_deadline.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <span>
#include <string>

namespace {

using svp::builder::workers::StreamDeadline;

// Short enough to keep the test quick; the read it bounds would otherwise
// never return, so any positive limit proves the cancel.
constexpr std::chrono::milliseconds kShortLimit{50};
// Far beyond the test's run time: this deadline must never fire.
constexpr std::chrono::milliseconds kLongLimit = std::chrono::hours{1};

// A peer that never answers: read_some blocks until cancel().
class SilentStream final : public svp::exec::remote::RemoteStream {
 public:
  std::size_t read_some(std::span<std::byte>) override {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return cancelled_; });
    return 0;
  }
  void write_all(std::span<const std::byte>) override {}
  void cancel() override {
    {
      const std::lock_guard lock(mutex_);
      cancelled_ = true;
    }
    changed_.notify_all();
  }
  [[nodiscard]] svp::exec::remote::TlsSession tls_session() const override { return {}; }
  [[nodiscard]] std::string peer_description() const override { return "silent"; }
  [[nodiscard]] bool cancelled() const {
    const std::lock_guard lock(mutex_);
    return cancelled_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool cancelled_ = false;
};

void test_expiry_cancels_a_blocked_read() {
  SilentStream stream;
  const StreamDeadline deadline(stream, kShortLimit);
  std::byte buffer[1];
  assert(stream.read_some(buffer) == 0);  // returns only once cancelled
  assert(deadline.expired());
  assert(stream.cancelled());
}

void test_ending_first_leaves_the_stream_open() {
  SilentStream stream;
  {
    const StreamDeadline deadline(stream, kLongLimit);
    assert(!deadline.expired());
  }
  assert(!stream.cancelled());
}

}  // namespace

int main() {
  test_expiry_cancels_a_blocked_read();
  test_ending_first_leaves_the_stream_open();
  return 0;
}
