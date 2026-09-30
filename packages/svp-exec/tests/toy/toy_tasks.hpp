#pragma once

// Toy task type shared by the scheduler tests (in-process) and the
// svp-exec-test-worker executable (loopback). Output bytes depend only on
// task_id, `seed`, and the content of the task's inputs (each input file is
// read and its BLAKE3 folded into the output), so every executor must produce
// identical outputs.
//
// Parameters (canonical JSON object):
//   seed         uint, required
//   fault        optional, one of toy_fault_name(): a test-only failure mode
//   once_marker  optional path: the fault fires only if this file does not
//                exist yet, and creates it (so a retry succeeds)
//   sleep_ms     optional uint: real sleep before producing output
//
// Faults simulate a misbehaving worker, not task logic, so they fire only
// when the registry was built with ToyFaults::honoured (the test worker);
// the in-process registry ignores them and behaves normally.
//
// The attempt's cancellation token is checked before and after the sleep and
// before writing output.

#include "in_memory_artifact_store.hpp"
#include "svp/exec/task_graph.hpp"
#include "svp/exec/task_registry.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::test {

inline constexpr std::string_view kToyTaskType = "toy.digest";
inline constexpr std::string_view kToyBuildSession = "bs_toy";
// Diagnostic flag the test worker's relay looks for to corrupt a RESULT
// payload in transit.
inline constexpr std::string_view kToyCorruptInTransit = "toy_corrupt_in_transit";
// Exit status of the `crash` fault; any non-zero status would do.
inline constexpr int kToyCrashExitStatus = 70;

enum class ToyFault {
  none,
  crash,               // the worker process exits mid-task
  corrupt_in_transit,  // a RESULT payload byte flips after the worker built it
  hang,                // the worker process stops (SIGSTOP): no heartbeats
  nondeterministic,    // output bytes differ on every run
  fail_retryable,      // failed TaskResult, retryable
  fail_permanent,      // failed TaskResult, not retryable
  stall,               // loops until the attempt is cancelled, heartbeats go on
};

[[nodiscard]] std::string_view toy_fault_name(ToyFault fault);

enum class ToyFaults { honoured, ignored };

struct ToyTaskOptions {
  ToyFaults faults = ToyFaults::ignored;
  // Random real sleep in [0, max_jitter] before each task, to shuffle
  // completion order without touching outputs.
  std::chrono::microseconds max_jitter{0};
};

// Where a toy task stores its output bytes; returns their ref.
using ToyOutputWriter = std::function<ArtifactRef(std::vector<std::byte> bytes,
                                                  std::string media_type, std::string role)>;

void register_toy_tasks(TaskTypeRegistry& registry, ToyOutputWriter write_output,
                        ToyTaskOptions options);
void register_toy_tasks(TaskTypeRegistry& registry, InMemoryArtifactStore& store,
                        ToyTaskOptions options);

struct ToyTask {
  std::string task_id;
  std::vector<std::string> depends_on;
  std::uint64_t seed = 0;
  TaskOrderKey order_key;
  ToyFault fault = ToyFault::none;
  std::optional<std::filesystem::path> once_marker;
  std::uint64_t sleep_ms = 0;
  std::uint64_t est_seconds = 1;
  std::map<std::string, ArtifactRef> inputs;
};

[[nodiscard]] TaskNode make_toy_node(const ToyTask& task);

// The bytes a correct execution of `task_id` with `seed` produces, given the
// content of each input by name.
[[nodiscard]] std::string toy_expected_output(
    std::string_view task_id, std::uint64_t seed,
    const std::map<std::string, std::string>& input_contents = {});

}  // namespace svp::exec::test
