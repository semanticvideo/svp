#include "journal_test_support.hpp"
#include "svp/exec/source_fingerprint.hpp"

#include <algorithm>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-journal-resume-tests";

const ResumedTask& find_task(const ResumeReport& report, std::string_view task_id) {
  const auto found = std::find_if(report.tasks.begin(), report.tasks.end(),
                                  [&](const ResumedTask& task) { return task.task_id == task_id; });
  if (found == report.tasks.end()) {
    throw std::runtime_error("resume report lacks " + std::string(task_id));
  }
  return *found;
}

void expect_state(const ResumeReport& report, std::string_view task_id, TaskState expected) {
  const TaskState actual = find_task(report, task_id).state;
  expect_equal(task_state_name(actual), task_state_name(expected),
               "resumed state of " + std::string(task_id));
}

// Simulated power cut for the current process: no destructors, no WAL
// checkpoint, no lock release except by the kernel.
[[noreturn]] void crash_now() {
  ::kill(::getpid(), SIGKILL);
  ::_exit(3);  // unreachable
}

// Runs `work` in a child process; `work` must end by calling crash_now()
// while its journal is still open.
template <typename Work>
void run_until_crash(Work&& work) {
  const pid_t child = ::fork();
  if (child == 0) {
    try {
      work();
    } catch (...) {
      ::_exit(2);
    }
    ::_exit(4);  // returned without crashing
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "child build was killed mid-flight (not an early failure)");
}

// DAG: A <- B <- C, A <- D. A committed; B half-recorded (result received,
// never committed) with an open attempt; a blob was mid-write in pending and
// another renamed into completed without its row.
void test_resume_after_crash() {
  JournalFixture fixture(kSuite);
  run_until_crash([&] {
    RecoveryJournal journal = fixture.create();
    journal.record_task(task_record("task.a"));
    journal.record_task(task_record("task.b", {"task.a"}));
    journal.record_task(task_record("task.c", {"task.b"}));
    journal.record_task(task_record("task.d", {"task.a"}));
    run_and_commit(journal, "task.a", {bytes_artifact("artifact_a", "output a")});
    journal.record_worker_session(WorkerSessionRecord{.worker_session_id = "ws_0001",
                                                      .worker_id = "worker_a",
                                                      .started_utc = "2026-09-30T12:00:00.000Z"});
    journal.set_task_state("task.b", TaskState::ready);
    journal.set_task_state("task.b", TaskState::leased);
    journal.record_task_attempt(TaskAttemptRecord{.task_id = "task.b",
                                                  .attempt = 1,
                                                  .worker_session_id = "ws_0001",
                                                  .outcome = AttemptOutcome::running});
    journal.set_task_state("task.b", TaskState::running);
    journal.set_task_state("task.b", TaskState::result_received);
    write_file(journal.layout().pending / "99-1-0.pending", "half of output b");
    write_file(journal.layout().completed / blake3_hex(blake3_digest("output b")), "output b");
    crash_now();
  });

  const JournalLayout layout = journal_layout_for(fixture.output_path);
  expect(fs::exists(layout.root / "build.sqlite-wal"), "the crash left an uncheckpointed WAL");

  RecoveryJournal journal = RecoveryJournal::open(fixture.output_path, fixed_clock_options());
  const ResumeReport report = journal.resume(fixture.sources());

  expect_state(report, "task.a", TaskState::committed);
  expect_state(report, "task.b", TaskState::ready);
  expect_state(report, "task.c", TaskState::planned);
  expect_state(report, "task.d", TaskState::ready);
  expect(find_task(report, "task.b").depends_on == std::vector<std::string>{"task.a"},
         "dependencies reconstructed");
  expect(report.demoted.empty(), "intact committed outputs are kept");
  expect(report.discarded_pending_blobs == 1, "pending leftovers discarded");
  expect(report.discarded_completed_blobs == 1, "unreferenced completed blob discarded");
  expect(report.abandoned_attempts == 1, "open attempt abandoned");
  expect(count_entries(layout.pending) == 0, "pending directory empty");
  expect(count_entries(layout.completed) == 1, "only the committed blob remains");
  expect(journal.task_state("task.b") == TaskState::ready, "journal rows match the report");

  JournalDatabase database(layout.database);
  expect_equal(database.value("SELECT outcome FROM task_attempt"), "abandoned",
               "attempt outcome persisted");
  expect_equal(database.value("SELECT status FROM worker_session"), "lost",
               "active worker sessions are lost after a coordinator crash");

  // The build continues from the reconstructed state.
  journal.set_task_state("task.b", TaskState::running);
  journal.set_task_state("task.b", TaskState::result_received);
  journal.commit_task(TaskCommit{.task_id = "task.b",
                                 .output_blake3 = blake3_digest("output of task.b"),
                                 .artifacts = {bytes_artifact("artifact_b", "output b")}});
  expect(journal.task_state("task.b") == TaskState::committed, "resumed task commits");
}

void test_tampered_artifact_demoted() {
  JournalFixture fixture(kSuite);
  const std::string content_a = "output a";
  const std::string content_b = "output b";
  const std::string content_c = "output c";
  {
    RecoveryJournal journal = fixture.create();
    journal.record_task(task_record("task.a"));
    journal.record_task(task_record("task.b", {"task.a"}));
    journal.record_task(task_record("task.c"));
    run_and_commit(journal, "task.a", {bytes_artifact("artifact_a", content_a)});
    journal.set_task_state("task.b", TaskState::ready);
    journal.set_task_state("task.b", TaskState::result_received);
    journal.commit_task(TaskCommit{
        .task_id = "task.b",
        .output_blake3 = blake3_digest("output of task.b"),
        .artifacts = {bytes_artifact("artifact_b", content_b)},
        .cache_hit = CacheHitRecord{.cache_key = blake3_digest("key b"),
                                    .artifact_id = "artifact_b",
                                    .verified = true}});
    run_and_commit(journal, "task.c", {bytes_artifact("artifact_c", content_c)});
  }
  const JournalLayout layout = journal_layout_for(fixture.output_path);
  // Same length, different bytes for B; C's blob disappears.
  write_file(layout.completed / blake3_hex(blake3_digest(content_b)), "OUTPUT B");
  fs::remove(layout.completed / blake3_hex(blake3_digest(content_c)));

  RecoveryJournal journal = RecoveryJournal::open(fixture.output_path);
  const ResumeReport report = journal.resume(fixture.sources());

  expect(report.demoted.size() == 2, "both invalid tasks demoted");
  expect(report.demoted[0].task_id == "task.b" && report.demoted[0].artifact_id == "artifact_b" &&
             report.demoted[0].reason == DemotionReason::artifact_hash_mismatch,
         "tampered blob demotes its task");
  expect(report.demoted[1].task_id == "task.c" &&
             report.demoted[1].reason == DemotionReason::artifact_missing,
         "missing blob demotes its task");
  expect_state(report, "task.a", TaskState::committed);
  expect_state(report, "task.b", TaskState::ready);
  expect_state(report, "task.c", TaskState::ready);
  expect(!fs::exists(layout.completed / blake3_hex(blake3_digest(content_b))),
         "tampered bytes are deleted, never trusted");
  expect(report.discarded_completed_blobs == 1, "the tampered blob is counted as discarded");

  JournalDatabase database(layout.database);
  expect_equal(database.value("SELECT count(*) FROM artifact WHERE task_id IN ('task.b','task.c')"),
               "0", "demoted artifact rows removed");
  expect_equal(database.value("SELECT count(*) FROM artifact_provenance"), "1",
               "only task.a provenance remains");
  expect_equal(database.value("SELECT count(*) FROM cache_hit"), "0",
               "cache hit of a demoted task removed");
  expect_equal(database.value("SELECT output_blake3, completed_utc FROM task WHERE task_id = "
                              "'task.b'"),
               "NULL\tNULL", "demoted task loses its output digest");

  // The same artifact IDs can be committed again with good bytes.
  journal.set_task_state("task.b", TaskState::running);
  journal.set_task_state("task.b", TaskState::result_received);
  journal.commit_task(TaskCommit{.task_id = "task.b",
                                 .output_blake3 = blake3_digest("output of task.b"),
                                 .artifacts = {bytes_artifact("artifact_b", content_b)}});
  expect(journal.task_state("task.b") == TaskState::committed, "demoted task recommits");
}

void test_source_mismatch_rejected() {
  JournalFixture fixture(kSuite);
  { RecoveryJournal journal = fixture.create(); }
  RecoveryJournal journal = RecoveryJournal::open(fixture.output_path);

  SourceFingerprintRecord changed = fixture.source;
  changed.blake3 = blake3_digest("re-encoded movie");
  expect_journal_error(JournalErrorCode::source_mismatch,
                       [&] { (void)journal.resume(std::span(&changed, 1)); },
                       "changed source content");
  changed = fixture.source;
  changed.size_bytes += 1;
  expect_journal_error(JournalErrorCode::source_mismatch,
                       [&] { (void)journal.resume(std::span(&changed, 1)); },
                       "changed source size");
  expect_journal_error(JournalErrorCode::source_mismatch,
                       [&] { (void)journal.resume({}); }, "journaled source not supplied");
  const std::vector<SourceFingerprintRecord> extra = {
      fixture.source, SourceFingerprintRecord{.source_id = "source_001",
                                              .path = "/other.mov",
                                              .size_bytes = 1,
                                              .blake3 = blake3_digest("x")}};
  expect_journal_error(JournalErrorCode::source_mismatch, [&] { (void)journal.resume(extra); },
                       "unexpected extra source");

  // Moving or touching the file does not change its identity.
  SourceFingerprintRecord moved = fixture.source;
  moved.path = "/new/location/input.mov";
  moved.mtime_ns = 999;
  (void)journal.resume(std::span(&moved, 1));
}

void test_fingerprint_source() {
  JournalFixture fixture(kSuite);
  const SourceFingerprintRecord fingerprint =
      fingerprint_source("source_000", fixture.source_path);
  expect(fingerprint.blake3 == fixture.source.blake3, "fingerprint hashes the file");
  expect(fingerprint.size_bytes == fixture.source.size_bytes, "fingerprint records size");
  expect(fingerprint.mtime_ns.has_value(), "fingerprint records mtime");
  expect_journal_error(JournalErrorCode::io_error,
                       [&] { (void)fingerprint_source("s", fixture.scratch.path / "missing"); },
                       "missing source");
}

}  // namespace

int main() {
  return run_tests(kSuite, {
                               {"resume_after_crash", test_resume_after_crash},
                               {"tampered_artifact_demoted", test_tampered_artifact_demoted},
                               {"source_mismatch_rejected", test_source_mismatch_rejected},
                               {"fingerprint_source", test_fingerprint_source},
                           });
}
