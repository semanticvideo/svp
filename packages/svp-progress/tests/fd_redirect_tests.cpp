// Progress written through a stable stderr duplicate must survive code that
// temporarily points STDERR_FILENO at /dev/null (library log silencing) while
// another thread keeps emitting progress. Every mode that reaches a terminal
// or pipe (json, plain, auto/tty) is checked: each emitted sequence number
// must arrive exactly once, in emission order.

#include "svp/progress/fd_stream.hpp"
#include "svp/progress/renderer.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace {

using svp::progress::Event;
using svp::progress::EventKind;
using svp::progress::Mode;

// Events emitted while the test itself holds STDERR_FILENO on /dev/null. This
// phase guarantees overlap with a redirection regardless of scheduling.
constexpr std::uint64_t kEventsWhileRedirected = 64;
// Events emitted while a second thread toggles STDERR_FILENO between /dev/null
// and the real destination as fast as it can. Large enough that many emits
// land inside redirect windows on any scheduler.
constexpr std::uint64_t kEventsWhileToggling = 4000;
constexpr std::uint64_t kTotalEvents =
    kEventsWhileRedirected + kEventsWhileToggling;

// Delimited so "<seq:1>" can never match inside "<seq:12>".
std::string seq_token(std::uint64_t seq) {
  return "<seq:" + std::to_string(seq) + ">";
}

Event numbered_event(std::uint64_t seq) {
  // Warnings are rendered with their message by every sink, including the tty
  // history row, so the token is observable in all modes.
  return Event{.kind = EventKind::warning,
               .stage_id = "redirect_probe",
               .stage_label = "Redirect Probe",
               .message = seq_token(seq),
               .t_ms = static_cast<std::int64_t>(seq),
               .seq = seq};
}

class TempFile {
 public:
  TempFile() {
    const std::filesystem::path pattern =
        std::filesystem::temp_directory_path() / "svp-progress-redirect-XXXXXX";
    std::string buffer = pattern.string();
    fd_ = ::mkstemp(buffer.data());
    assert(fd_ >= 0);
    path_ = buffer;
  }
  ~TempFile() {
    ::close(fd_);
    std::filesystem::remove(path_);
  }
  int fd() const { return fd_; }
  std::string contents() const {
    std::ifstream input(path_, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }

 private:
  int fd_ = -1;
  std::filesystem::path path_;
};

void assert_every_seq_once_in_order(const std::string& output,
                                    std::string_view mode_label) {
  std::size_t cursor = 0;
  for (std::uint64_t seq = 1; seq <= kTotalEvents; ++seq) {
    const std::string token = seq_token(seq);
    const std::size_t first = output.find(token);
    if (first == std::string::npos) {
      std::cerr << mode_label << ": missing " << token << "\n";
      std::abort();
    }
    if (output.find(token, first + token.size()) != std::string::npos) {
      std::cerr << mode_label << ": duplicated " << token << "\n";
      std::abort();
    }
    if (first < cursor) {
      std::cerr << mode_label << ": out of order " << token << "\n";
      std::abort();
    }
    cursor = first;
  }
}

// Counts `"seq":<n>` fields whose number is exactly `seq` (not a prefix of a
// longer number).
std::size_t count_json_seq(const std::string& output, std::uint64_t seq) {
  const std::string field = "\"seq\":" + std::to_string(seq);
  std::size_t count = 0;
  for (std::size_t at = output.find(field); at != std::string::npos;
       at = output.find(field, at + field.size())) {
    const std::size_t next = at + field.size();
    if (next >= output.size() || output[next] < '0' || output[next] > '9') {
      ++count;
    }
  }
  return count;
}

void run_mode(Mode mode, bool is_tty, std::string_view mode_label) {
  const int saved_stderr = ::dup(STDERR_FILENO);
  const int devnull = ::open("/dev/null", O_WRONLY);
  assert(saved_stderr >= 0 && devnull >= 0);

  // The "terminal" for this run: fd 2 points here when the stream is captured.
  TempFile destination;
  ::dup2(destination.fd(), STDERR_FILENO);

  std::string stray_marker;
  {
    svp::progress::FdStream progress_stream(STDERR_FILENO);
    assert(progress_stream.fd() >= 0);
    assert(progress_stream.fd() != STDERR_FILENO);
    auto sink = svp::progress::make_sink(mode, progress_stream.stream(),
                                         is_tty, progress_stream.fd());

    std::uint64_t next_seq = 1;

    // Phase 1: fd 2 is definitely on /dev/null for every emit.
    ::dup2(devnull, STDERR_FILENO);
    stray_marker = "stray-stderr-write-" + std::string(mode_label);
    std::cerr << stray_marker << std::endl;
    for (std::uint64_t i = 0; i < kEventsWhileRedirected; ++i) {
      sink->emit(numbered_event(next_seq++));
    }
    ::dup2(destination.fd(), STDERR_FILENO);

    // Phase 2: another thread toggles fd 2 while this thread emits.
    std::atomic<bool> emitting{true};
    std::atomic<std::uint64_t> toggles{0};
    std::thread redirector([&] {
      while (emitting.load(std::memory_order_acquire)) {
        ::dup2(devnull, STDERR_FILENO);
        std::this_thread::yield();
        ::dup2(destination.fd(), STDERR_FILENO);
        toggles.fetch_add(1, std::memory_order_relaxed);
      }
    });
    while (toggles.load(std::memory_order_relaxed) == 0) {
      std::this_thread::yield();
    }
    for (std::uint64_t i = 0; i < kEventsWhileToggling; ++i) {
      sink->emit(numbered_event(next_seq++));
    }
    emitting.store(false, std::memory_order_release);
    redirector.join();
    assert(next_seq == kTotalEvents + 1);
  }

  ::dup2(saved_stderr, STDERR_FILENO);
  ::close(saved_stderr);
  ::close(devnull);

  const std::string output = destination.contents();
  // Proves the redirection really hid fd 2: a plain stderr write is lost, so
  // a sink writing to fd 2 directly would have lost events too.
  assert(output.find(stray_marker) == std::string::npos);
  assert_every_seq_once_in_order(output, mode_label);
  if (mode == Mode::json) {
    for (std::uint64_t seq = 1; seq <= kTotalEvents; ++seq) {
      assert(count_json_seq(output, seq) == 1);
    }
  }
}

void closed_descriptor_discards_without_throwing() {
  svp::progress::FdStream closed(-1);
  assert(closed.fd() == -1);
  closed.stream() << "discarded\n";
  assert(!closed.stream().good());
}

}  // namespace

int main() {
  run_mode(Mode::json, false, "json");
  run_mode(Mode::plain, false, "plain");
  run_mode(Mode::auto_, true, "auto-tty");
  run_mode(Mode::auto_, false, "auto-pipe");
  closed_descriptor_discards_without_throwing();
  return 0;
}
