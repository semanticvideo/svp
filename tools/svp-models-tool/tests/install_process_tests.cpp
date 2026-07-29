#include "install_process.hpp"

#include <cassert>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>

int main() {
  std::thread interrupt([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    kill(getpid(), SIGINT);
  });
  const auto result =
      svp::models::tool::run_process({"/bin/sleep", "10"});
  interrupt.join();
  assert(result.exit_code == 130);
  return 0;
}
