#pragma once

#include "svp/exec/blake3_digest.hpp"

#include <cstdint>
#include <string>

namespace svp::exec {

// Wall-clock phases of one attempt, integer milliseconds.
struct TaskTimingMs {
  std::uint64_t queue = 0;
  std::uint64_t input_fetch = 0;
  std::uint64_t decode = 0;
  std::uint64_t compute = 0;
  std::uint64_t encode = 0;

  bool operator==(const TaskTimingMs&) const = default;
};

// Process CPU time consumed by one attempt, integer milliseconds.
struct TaskCpuMs {
  std::uint64_t user = 0;
  std::uint64_t system = 0;

  bool operator==(const TaskCpuMs&) const = default;
};

// Where and how an attempt ran (plan §4.2). Journal and run-report data only;
// it never enters a package (plan §4.7). JSON form:
//   {"cpu_ms":{"system","user"}, "peak_rss_bytes",
//    "runtime_id":"b3:<hex>",
//    "timing_ms":{"compute","decode","encode","input_fetch","queue"},
//    "worker_session_id"}
struct TaskExecution {
  std::string worker_session_id;
  Blake3Digest runtime_id{};
  TaskTimingMs timing_ms;
  TaskCpuMs cpu_ms;
  std::uint64_t peak_rss_bytes = 0;

  bool operator==(const TaskExecution&) const = default;
};

}  // namespace svp::exec
