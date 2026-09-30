#include "scheduler_test_support.hpp"
#include "svp/exec/frame_decoder.hpp"
#include "svp/exec/lease_frames.hpp"
#include "svp/exec/lease_policy.hpp"
#include "svp/exec/retry_policy.hpp"
#include "svp/exec/task_attempt_runner.hpp"
#include "svp/exec/task_graph.hpp"

#include <algorithm>
#include <limits>
#include <random>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

ToyTask toy(std::string task_id, std::vector<std::string> depends_on, std::uint64_t ordinal,
            std::uint64_t est_seconds = 1) {
  return ToyTask{.task_id = std::move(task_id),
                 .depends_on = std::move(depends_on),
                 .seed = ordinal,
                 .order_key = {.lane = "lane", .ordinals = {ordinal}},
                 .est_seconds = est_seconds};
}

template <typename Function>
void expect_graph_issue(TaskGraphIssue expected, Function&& function,
                        std::string_view message) {
  try {
    function();
  } catch (const TaskGraphError& error) {
    expect(error.issue() == expected,
           std::string(message) + ": got " + std::string(task_graph_issue_name(error.issue())));
    expect(error.code() == ExecErrorCode::invalid_value, "graph errors are invalid_value");
    return;
  }
  throw std::runtime_error(std::string(message) + ": no TaskGraphError");
}

void test_graph_validation() {
  expect_graph_issue(TaskGraphIssue::duplicate_task_id,
                     [] { make_toy_graph({toy("t.a", {}, 0), toy("t.a", {}, 1)}); },
                     "duplicate id");
  expect_graph_issue(TaskGraphIssue::missing_dependency,
                     [] { make_toy_graph({toy("t.a", {"t.zzz"}, 0)}); }, "missing dependency");
  expect_graph_issue(TaskGraphIssue::dependency_cycle,
                     [] {
                       make_toy_graph({toy("t.a", {"t.c"}, 0), toy("t.b", {"t.a"}, 1),
                                       toy("t.c", {"t.b"}, 2), toy("t.d", {}, 3)});
                     },
                     "three-node cycle");
  expect_graph_issue(TaskGraphIssue::dependency_cycle,
                     [] { make_toy_graph({toy("t.a", {"t.a"}, 0)}); }, "self dependency");
  expect_graph_issue(TaskGraphIssue::duplicate_order_key,
                     [] { make_toy_graph({toy("t.a", {}, 7), toy("t.b", {}, 7)}); },
                     "duplicate order key");
  expect_graph_issue(TaskGraphIssue::mixed_build_sessions,
                     [] {
                       TaskNode other = make_toy_node(toy("t.b", {}, 1));
                       other.spec.build_session_id = "bs_other";
                       TaskGraph({make_toy_node(toy("t.a", {}, 0)), other});
                     },
                     "mixed sessions");
  expect_graph_issue(TaskGraphIssue::invalid_task_spec,
                     [] {
                       TaskNode bad = make_toy_node(toy("t.a", {}, 0));
                       bad.spec.parameters_blake3 = repeated_digest(0x01);
                       TaskGraph({bad});
                     },
                     "invalid spec");

  const TaskGraph empty({});
  expect(empty.size() == 0, "empty graph is valid");
  expect(ReadySet(empty).all_complete(), "empty graph is complete");
}

void test_ready_set_and_critical_path() {
  //   a(3) -> c(1) -> d(2)
  //   b(1) -> d
  const TaskGraph graph = make_toy_graph({toy("t.a", {}, 0, 3), toy("t.b", {}, 1, 1),
                                          toy("t.c", {"t.a"}, 2, 1),
                                          toy("t.d", {"t.b", "t.c"}, 3, 2)});
  expect(graph.critical_path_seconds(*graph.find("t.a")) == 6, "a: 3+1+2");
  expect(graph.critical_path_seconds(*graph.find("t.b")) == 3, "b: 1+2");
  expect(graph.critical_path_seconds(*graph.find("t.c")) == 3, "c: 1+2");
  expect(graph.critical_path_seconds(*graph.find("t.d")) == 2, "d: 2");
  expect(!graph.find("t.zzz"), "unknown id");

  ReadySet ready(graph);
  expect(ready.initially_ready() == std::vector<std::size_t>{0, 1}, "roots ready");
  expect(ready.complete(1).empty(), "d still waits for c");
  expect(ready.complete(0) == std::vector<std::size_t>{2}, "c ready after a");
  expect(ready.complete(2) == std::vector<std::size_t>{3}, "d ready after b and c");
  expect(!ready.all_complete(), "d pending");
  expect_exec_error(ExecErrorCode::invalid_value, [&] { ready.complete(2); },
                    "double completion");
  static_cast<void>(ready.complete(3));
  expect(ready.all_complete(), "all complete");
}

void test_order_key_ordering() {
  const TaskOrderKey a{.lane = "alpha", .ordinals = {2}};
  const TaskOrderKey b{.lane = "alpha", .ordinals = {10}};
  const TaskOrderKey c{.lane = "alpha", .ordinals = {10, 0}};
  const TaskOrderKey d{.lane = "beta", .ordinals = {0}};
  expect(a < b && b < c && c < d, "lane then ordinals, numerically");
  expect(!(b < a) && !(a < a), "strict order");
}

void test_ordered_reduction() {
  const TaskGraph graph = make_toy_graph(two_lane_toy_tasks(6));
  ToyRuntime runtime;
  std::vector<CommittedResult> results;
  for (std::size_t index = 0; index < graph.size(); ++index) {
    const TaskSpec& spec = graph.node(index).spec;
    AttemptOutput output = run_task_attempt(runtime.registry, runtime.store, spec,
                                            AttemptContext{.attempt = 1,
                                                           .worker_session_id = "ws_test"});
    results.push_back(CommittedResult{.result = std::move(output.result),
                                      .payloads = std::move(output.payloads),
                                      .executor_id = "test"});
  }
  std::mt19937 generator(7);
  std::vector<CommittedResult> shuffled = results;
  std::shuffle(shuffled.begin(), shuffled.end(), generator);

  const auto ordered = results_in_canonical_order(graph, "alpha", shuffled);
  expect(ordered.size() == 6, "six alpha results");
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    expect(graph.node(*graph.find(ordered[index]->result.task_id)).order_key.ordinals ==
               std::vector<std::uint64_t>{index},
           "alpha results in ordinal order");
  }
  expect(reduce_lane(graph, "beta", shuffled) == reduce_lane(graph, "beta", results),
         "reduction independent of input order");
  expect(results_in_canonical_order(graph, "gamma", shuffled).empty(), "empty lane");

  std::vector<CommittedResult> missing = shuffled;
  missing.erase(std::find_if(missing.begin(), missing.end(), [](const CommittedResult& r) {
    return r.result.task_id == "task.toy.a_003";
  }));
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(results_in_canonical_order(graph, "alpha", missing)); },
                    "gap in lane");
  std::vector<CommittedResult> duplicated = shuffled;
  duplicated.push_back(shuffled.front());
  expect_exec_error(
      ExecErrorCode::invalid_value,
      [&] { static_cast<void>(results_in_canonical_order(graph, "alpha", duplicated)); },
      "duplicate result");
  std::vector<CommittedResult> unknown = shuffled;
  unknown.back().result.task_id = "task.toy.zzz";
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(results_in_canonical_order(graph, "alpha", unknown)); },
                    "unknown task");
}

void test_lease_policy() {
  LeasePolicy policy;
  validate_lease_policy(policy);
  expect(lease_duration(policy, 0) == kDefaultLeaseFloor, "floor for tiny tasks");
  expect(lease_duration(policy, 60) == std::chrono::milliseconds(120'000),
         "factor x estimate above the floor");
  expect(lease_duration(policy, std::numeric_limits<std::uint64_t>::max()).count() ==
             std::numeric_limits<std::chrono::milliseconds::rep>::max(),
         "saturates");

  LeasePolicy slow_heartbeat = policy;
  slow_heartbeat.heartbeat_interval = policy.lease_floor / 3;
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { validate_lease_policy(slow_heartbeat); },
                    "heartbeat must be below floor / 3");
  LeasePolicy zero_factor = policy;
  zero_factor.lease_factor = 0;
  expect_exec_error(ExecErrorCode::invalid_value, [&] { validate_lease_policy(zero_factor); },
                    "zero factor");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { validate_retry_policy(RetryPolicy{.max_attempts = 0}); },
                    "zero attempts");
  SchedulerPolicy no_wait;
  no_wait.max_idle_wait = std::chrono::milliseconds(0);
  expect_exec_error(ExecErrorCode::invalid_value, [&] { validate_scheduler_policy(no_wait); },
                    "zero idle wait");
}

void test_lease_frames() {
  const TaskSpec spec = make_toy_node(toy("t.a", {}, 0)).spec;
  const Lease lease{.lease_id = "lease_7",
                    .attempt = 2,
                    .duration = std::chrono::milliseconds(30'000),
                    .heartbeat_interval = std::chrono::milliseconds(5'000)};
  const Frame assign = make_leased_assign_frame(spec, lease);
  const Frame decoded = decode_frame(encode_frame(assign));
  const LeasedAssignment parsed = leased_assignment_from_frame(decoded);
  expect(parsed.spec == spec && parsed.lease == lease, "ASSIGN round trip");

  expect(lease_id_from_heartbeat_frame(decode_frame(encode_frame(make_heartbeat_frame("lease_7")))) ==
             "lease_7",
         "HEARTBEAT round trip");
  expect(lease_id_from_cancel_frame(make_cancel_frame("lease_7")) == "lease_7",
         "CANCEL round trip");
  expect(make_shutdown_frame().type == MessageType::shutdown, "SHUTDOWN");

  Frame extra = assign;
  extra.body["lease"]["surprise"] = 1;
  expect_exec_error(ExecErrorCode::unknown_field,
                    [&] { static_cast<void>(leased_assignment_from_frame(extra)); },
                    "unknown lease field");
  Frame zero_attempt = assign;
  zero_attempt.body["lease"]["attempt"] = 0;
  expect_exec_error(ExecErrorCode::invalid_value,
                    [&] { static_cast<void>(leased_assignment_from_frame(zero_attempt)); },
                    "attempt 0");
  expect_exec_error(ExecErrorCode::frame_malformed,
                    [&] { static_cast<void>(lease_id_from_cancel_frame(make_heartbeat_frame("x"))); },
                    "wrong type");
  expect_exec_error(ExecErrorCode::invalid_value,
                    [] { static_cast<void>(make_heartbeat_frame("bad id")); }, "bad lease id");
}

}  // namespace

int main() {
  return run_tests("svp-exec-task-graph-tests",
                   {
                       {"graph validation", test_graph_validation},
                       {"ready set and critical path", test_ready_set_and_critical_path},
                       {"order key ordering", test_order_key_ordering},
                       {"ordered reduction", test_ordered_reduction},
                       {"lease policy", test_lease_policy},
                       {"lease frames", test_lease_frames},
                   });
}
