#pragma once

#include <atomic>
#include <chrono>

namespace svp::exec {

// Monotonic time source for scheduler decisions (lease expiry). Injected so
// tests decide exactly when a lease expires instead of racing wall time.
// Values are milliseconds since an arbitrary, clock-specific origin; only
// differences are meaningful.
class Clock {
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual std::chrono::milliseconds now() const = 0;
};

// std::chrono::steady_clock; the production clock.
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] std::chrono::milliseconds now() const override;
};

// Advances only when told to. Thread-safe.
class ManualClock final : public Clock {
 public:
  [[nodiscard]] std::chrono::milliseconds now() const override;
  void advance(std::chrono::milliseconds delta);

 private:
  std::atomic<std::chrono::milliseconds::rep> now_ms_{0};
};

}  // namespace svp::exec
