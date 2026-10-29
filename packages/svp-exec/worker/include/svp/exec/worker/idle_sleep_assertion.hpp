#pragma once

// Keeps the Mac from sleeping when idle while a worker serves a session.
//
// A session's work is not user activity, so a worker whose energy settings
// put it to sleep after a few idle minutes would sleep in the middle of a
// job: a worker set to sleep after 1 minute dropped its coordinator's
// connection part way through calibration ("worker connection ended").
// Each session holds an IOKit PreventUserIdleSystemSleep assertion, named
// for the session, so `pmset -g assertions` shows why the Mac stays awake;
// the display may still sleep, and the Mac still sleeps when told to. The
// assertion goes when the session ends, so an idle worker sleeps as its
// settings say. Best effort: a session whose assertion is refused still runs.

#include <cstdint>
#include <string>

namespace svp::exec::worker {

class IdleSleepAssertion {
 public:
  explicit IdleSleepAssertion(const std::string& name);
  ~IdleSleepAssertion();
  IdleSleepAssertion(const IdleSleepAssertion&) = delete;
  IdleSleepAssertion& operator=(const IdleSleepAssertion&) = delete;

  // Whether the system accepted the assertion.
  [[nodiscard]] bool held() const noexcept { return held_; }

 private:
  std::uint32_t id_ = 0;
  bool held_ = false;
};

}  // namespace svp::exec::worker
