#pragma once

// The leases each executor declined during one stage's scheduler run
// (svp::exec::AttemptEventKind::rejected: a worker's admission refused it
// for memory, memory pressure, or slots shared with other coordinators). A
// declined lease is not a failure, so the scheduler retries the task
// elsewhere without a word; without this tally a stage whose workers decline
// everything would run on this Mac alone, silently. Stages print summary()
// beside their per-executor totals.

#include "svp/exec/attempt_event.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace svp::builder::engine {

class DeclinedLeaseTally {
 public:
  // Counts `event` when it is a rejection; ignores every other kind.
  void observe(const svp::exec::AttemptEvent& event);

  [[nodiscard]] std::uint64_t declined(std::string_view executor_id) const;

  // One stderr line per executor that declined a lease:
  //   svp-builder: <stage>: <executor> declined <n> lease(s): <code> <n>, ...;
  //   last: <message>
  // Empty when nothing was declined.
  [[nodiscard]] std::string summary(std::string_view stage) const;

 private:
  struct Executor {
    std::uint64_t total = 0;
    std::map<std::string, std::uint64_t> by_code;
    std::string last_detail;
  };
  std::map<std::string, Executor, std::less<>> executors_;
};

}  // namespace svp::builder::engine
