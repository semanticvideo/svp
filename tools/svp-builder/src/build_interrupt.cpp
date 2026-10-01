#include "build_interrupt.hpp"

#include "staging_cleanup.hpp"
#include "svp/exec/cancellation_token.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace svp::builder {
namespace {

// Conventional shell status for a process ended by a signal.
constexpr int kSignalExitStatusBase = 128;

struct InterruptRegistry {
  std::mutex mutex;
  std::vector<svp::exec::CancellationToken*> builds;
  std::atomic<int> signal{0};
};

InterruptRegistry& registry() {
  static InterruptRegistry* instance = new InterruptRegistry();
  return *instance;
}

void write_stderr(const char* message) {
  const std::size_t length = std::char_traits<char>::length(message);
  [[maybe_unused]] const ssize_t written = ::write(STDERR_FILENO, message, length);
}

[[noreturn]] void exit_now(int signal_number) {
  staging_cleanup_internal::cleanup_registered_auto_staging();
  std::_Exit(kSignalExitStatusBase + signal_number);
}

void handle_signals(sigset_t signals) {
  int signal_number = 0;
  if (sigwait(&signals, &signal_number) != 0) {
    return;
  }
  InterruptRegistry& state = registry();
  {
    const std::lock_guard lock(state.mutex);
    if (state.builds.empty()) {
      exit_now(signal_number);
    }
    state.signal.store(signal_number);
    for (svp::exec::CancellationToken* build : state.builds) {
      build->request();
    }
  }
  write_stderr(
      "\nsvp-builder: interrupted; stopping the build and keeping its recovery "
      "journal for --resume (interrupt again to stop immediately)\n");
  std::thread([signal_number] {
    std::this_thread::sleep_for(kInterruptGracePeriod);
    write_stderr("svp-builder: stopping now; the recovery journal is kept for --resume\n");
    exit_now(signal_number);
  }).detach();

  int second_signal = 0;
  if (sigwait(&signals, &second_signal) == 0) {
    write_stderr("svp-builder: stopping now; the recovery journal is kept for --resume\n");
    exit_now(signal_number);
  }
}

}  // namespace

void install_build_interrupt_handling() {
  static const bool installed = [] {
    std::atexit(staging_cleanup_internal::cleanup_registered_auto_staging);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
      return true;
    }
    std::thread([signals] { handle_signals(signals); }).detach();
    return true;
  }();
  static_cast<void>(installed);
}

BuildInterruptScope::BuildInterruptScope(svp::exec::CancellationToken& cancellation)
    : cancellation_(cancellation) {
  InterruptRegistry& state = registry();
  const std::lock_guard lock(state.mutex);
  state.builds.push_back(&cancellation_);
  if (state.signal.load() != 0) {
    cancellation_.request();
  }
}

BuildInterruptScope::~BuildInterruptScope() {
  InterruptRegistry& state = registry();
  const std::lock_guard lock(state.mutex);
  state.builds.erase(std::remove(state.builds.begin(), state.builds.end(), &cancellation_),
                     state.builds.end());
}

int build_interrupt_signal() noexcept { return registry().signal.load(); }

}  // namespace svp::builder
