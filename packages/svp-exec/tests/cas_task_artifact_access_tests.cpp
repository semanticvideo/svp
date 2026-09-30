#include "scheduler_test_support.hpp"
#include "svp/exec/cas_task_artifact_access.hpp"
#include "svp/exec/task_attempt_runner.hpp"

#include <algorithm>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-cas-task-artifact-access-tests";

CasStore open_store(const fs::path& root, CasStoreOptions options = {}) {
  return expect_ok(CasStore::at(root, std::move(options)), "open store");
}

ArtifactRef put_text(CasTaskArtifactAccess& access, std::string_view text,
                     std::string role = "source_chunk") {
  return access.put(as_byte_span(text), "text/plain", std::move(role));
}

bool contains(const std::vector<Blake3Digest>& digests, const Blake3Digest& digest) {
  return std::find(digests.begin(), digests.end(), digest) != digests.end();
}

// A toy task with one input `chunk` resolved from the cache.
TaskSpec toy_spec_with_input(const ArtifactRef& chunk) {
  return make_toy_node(ToyTask{.task_id = "task.toy.cas",
                               .seed = 5,
                               .order_key = {.lane = "alpha", .ordinals = {0}},
                               .inputs = {{"chunk", chunk}}})
      .spec;
}

void test_inputs_resolve_to_verified_pinned_blobs() {
  TemporaryDirectory scratch(kSuite);
  CasTaskArtifactAccess access(open_store(scratch.path / "cache"), "bs_test");
  expect(access.pinned(), "the session holds a pin set");
  const ArtifactRef chunk = put_text(access, "chunk bytes 0");

  const TaskSpec spec = toy_spec_with_input(chunk);
  const ResolvedInputs resolved = access.resolve_inputs(spec);
  expect(resolved.size() == 1 && resolved.at("chunk").ref == chunk, "input resolved by name");
  expect_equal(read_file(resolved.at("chunk").path), "chunk bytes 0",
               "the path holds the input's bytes");
  expect(resolved.at("chunk").path.string().starts_with((scratch.path / "cache").string()),
         "the path is inside the cache root");
  expect(contains(access.pinned_digests(), chunk.blake3), "the input is pinned");
  expect(access.cache_warnings().empty(), "no cache warnings");
}

void test_missing_or_corrupt_input_is_unresolved() {
  TemporaryDirectory scratch(kSuite);
  CasTaskArtifactAccess access(open_store(scratch.path / "cache"), "bs_test");
  const std::string text = "never stored";
  const ArtifactRef absent = make_artifact_ref(as_byte_span(text), "text/plain", "source_chunk");
  expect_exec_error(ExecErrorCode::unresolved_input,
                    [&] { static_cast<void>(access.resolve_inputs(toy_spec_with_input(absent))); },
                    "missing input");

  const ArtifactRef stored = put_text(access, "original bytes");
  const ResolvedInputs resolved = access.resolve_inputs(toy_spec_with_input(stored));
  write_file(resolved.at("chunk").path, "tampered bytes");
  expect_exec_error(ExecErrorCode::unresolved_input,
                    [&] { static_cast<void>(access.resolve_inputs(toy_spec_with_input(stored))); },
                    "corrupt input");
  expect(!fs::exists(resolved.at("chunk").path), "the corrupt blob is removed, never served");
}

void test_outputs_round_trip_and_stay_pinned() {
  TemporaryDirectory scratch(kSuite);
  CasStoreOptions options;
  options.policy.max_bytes = 0;  // Evict everything that is not pinned.
  CasStore store = open_store(scratch.path / "cache", options);
  CasTaskArtifactAccess access(store, "bs_test");

  const ArtifactRef output = put_text(access, "output bytes", "toy_output");
  // An unrelated, unpinned blob written straight to the store.
  const std::string loose = "loose bytes";
  const Blake3Digest loose_digest = expect_ok(store.put(as_byte_span(loose)), "put loose");

  const EvictionReport report = expect_ok(store.evict(), "evict");
  expect(report.pinned_blobs_kept == 1, "the session's output survives eviction");
  expect(!store.has(loose_digest), "an unpinned blob is evicted");

  TaskResult result;
  result.task_id = "task.toy.cas";
  result.attempt = 1;
  result.outputs = {output};
  const std::vector<FramePayload> payloads = access.read_outputs(result);
  expect(payloads.size() == 1 && to_text(payloads.front()) == "output bytes",
         "read_outputs returns the stored bytes");
}

void test_cache_failures_fall_back_without_failing() {
  TemporaryDirectory scratch(kSuite);
  const fs::path root = scratch.path / "cache";
  CasTaskArtifactAccess first(open_store(root), "bs_test");
  // The same holder ID is live: the second access runs unpinned.
  CasTaskArtifactAccess second(open_store(root), "bs_test");
  expect(!second.pinned() && second.cache_warnings().size() == 1,
         "an unavailable pin set is a warning, not an error");

  if (::geteuid() == 0) {
    return;  // root ignores permission bits
  }
  // A read-only blob tree refuses new blobs: the output is kept in memory.
  fs::permissions(root / "blobs" / "b3", fs::perms::owner_write, fs::perm_options::remove);
  const ArtifactRef output = put_text(first, "cannot be cached", "toy_output");
  fs::permissions(root / "blobs" / "b3", fs::perms::owner_write, fs::perm_options::add);
  expect(first.memory_fallback_bytes() == output.bytes, "the output is held in memory");
  expect(!first.cache_warnings().empty(), "the failed put is reported");

  TaskResult result;
  result.task_id = "task.toy.cas";
  result.attempt = 1;
  result.outputs = {output};
  expect(to_text(first.read_outputs(result).front()) == "cannot be cached",
         "the held output is still served");
}

// The attempt runner, a toy task, and the cache together: the task reads its
// input through the resolved path and its output lands in the cache.
void test_attempt_through_the_cache() {
  TemporaryDirectory scratch(kSuite);
  CasTaskArtifactAccess access(open_store(scratch.path / "cache"), "bs_test");
  TaskTypeRegistry registry;
  register_toy_tasks(
      registry,
      [&access](std::vector<std::byte> bytes, std::string media_type, std::string role) {
        return access.put(bytes, std::move(media_type), std::move(role));
      },
      {});
  const ArtifactRef chunk = put_text(access, "chunk bytes 7");
  const TaskSpec spec = toy_spec_with_input(chunk);

  const AttemptOutput output = run_task_attempt(
      registry, access, spec, AttemptContext{.attempt = 1, .worker_session_id = "ws_test"},
      kNotCancelled);
  expect(output.result.status == TaskStatus::succeeded, "attempt succeeded");
  expect_equal(to_text(output.payloads.front()),
               toy_expected_output(spec.task_id, 5, {{"chunk", "chunk bytes 7"}}),
               "output folds in the resolved input's content");
  expect(access.store().has(output.result.outputs.front().blake3), "output stored in the cache");
  expect(contains(access.pinned_digests(), output.result.outputs.front().blake3),
         "output pinned for the session");

  CancellationToken cancelled;
  cancelled.request();
  const AttemptOutput stopped = run_task_attempt(
      registry, access, spec, AttemptContext{.attempt = 2, .worker_session_id = "ws_test"},
      cancelled);
  expect(stopped.result.status == TaskStatus::failed && stopped.result.error &&
             stopped.result.error->code == "cancelled" && stopped.result.error->retryable,
         "a cancelled attempt is a retryable cancelled failure");
}

}  // namespace

int main() {
  return run_tests(kSuite,
                   {
                       {"inputs resolve to verified pinned blobs",
                        test_inputs_resolve_to_verified_pinned_blobs},
                       {"missing or corrupt input is unresolved",
                        test_missing_or_corrupt_input_is_unresolved},
                       {"outputs round trip and stay pinned",
                        test_outputs_round_trip_and_stay_pinned},
                       {"cache failures fall back without failing",
                        test_cache_failures_fall_back_without_failing},
                       {"attempt through the cache", test_attempt_through_the_cache},
                   });
}
