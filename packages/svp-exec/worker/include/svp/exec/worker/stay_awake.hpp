#pragma once

// Keeps the Mac awake while a worker serves a session.
//
// A session's work is not user activity, so a worker whose energy settings
// put it to sleep after a few idle minutes would sleep in the middle of a
// job: a worker set to sleep after 1 minute dropped its coordinator's
// connection part way through calibration ("worker connection ended"). And
// a worker that was asleep when a coordinator reached it is only partly
// awake (dark wake): measured on a Mac mini, it went back to sleep 15 s
// after the traffic that woke it. So each session holds two IOKit
// assertions, both named for the session so `pmset -g assertions` shows why
// the Mac is awake:
//
//   PreventUserIdleSystemSleep  no idle sleep while fully awake (the
//                               display may still sleep).
//   PreventSystemSleep          on AC power the Mac stays in dark wake, or
//                               enters it, rather than sleep: a worker woken
//                               by its coordinator keeps running the job,
//                               and so does one told to sleep mid-job.
//
// Both go when the session ends, so an idle worker sleeps as its settings
// say. Best effort: a session whose assertions are refused still runs.

#include <cstdint>
#include <string>

namespace svp::exec::worker {

class StayAwake {
 public:
  explicit StayAwake(const std::string& name);
  ~StayAwake();
  StayAwake(const StayAwake&) = delete;
  StayAwake& operator=(const StayAwake&) = delete;

  // Whether the system accepted both assertions.
  [[nodiscard]] bool held() const noexcept { return idle_held_ && system_held_; }

 private:
  std::uint32_t idle_id_ = 0;
  std::uint32_t system_id_ = 0;
  bool idle_held_ = false;
  bool system_held_ = false;
};

}  // namespace svp::exec::worker
