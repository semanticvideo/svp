#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace svp::builder {

// Process-wide resource counters. `user_cpu_ms`/`system_cpu_ms` are
// getrusage(RUSAGE_SELF) and cover all threads, so concurrent stages share
// them. `children_*` are getrusage(RUSAGE_CHILDREN): CPU of terminated,
// waited-for subprocesses such as ffmpeg/ffprobe.
struct ProcessResourceSample {
  std::int64_t user_cpu_ms = 0;
  std::int64_t system_cpu_ms = 0;
  std::int64_t children_user_cpu_ms = 0;
  std::int64_t children_system_cpu_ms = 0;
  // High-water mark of resident set size for this process so far.
  std::int64_t peak_rss_bytes = 0;
};

using ProcessResourceSampler =
    std::function<std::optional<ProcessResourceSample>()>;

// Returns nullopt when the platform call fails.
std::optional<ProcessResourceSample> sample_process_resources();

}  // namespace svp::builder
