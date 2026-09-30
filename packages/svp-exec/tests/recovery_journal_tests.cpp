#include "journal_test_support.hpp"
#include "svp/exec/canonical_json.hpp"

#include <map>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-recovery-journal-tests";

// "name|type|notnull|pk" per column, in declaration order. The first eight
// tables are RC2 §20.4 verbatim; the last three are the plan §4.6 extension.
const std::map<std::string, std::vector<std::string>>& expected_schema() {
  static const std::map<std::string, std::vector<std::string>> schema = {
      {"build_session",
       {"id|TEXT|0|1", "started_utc|TEXT|1|0", "svp_version|TEXT|1|0",
        "builder_version|TEXT|1|0", "status|TEXT|1|0"}},
      {"source_fingerprint",
       {"source_id|TEXT|0|1", "path|TEXT|1|0", "size_bytes|INTEGER|1|0", "mtime_ns|INTEGER|0|0",
        "blake3|TEXT|1|0"}},
      {"task",
       {"task_id|TEXT|0|1", "task_type|TEXT|1|0", "status|TEXT|1|0", "cache_key|TEXT|0|0",
        "started_utc|TEXT|0|0", "completed_utc|TEXT|0|0", "output_blake3|TEXT|0|0"}},
      {"task_dependency", {"task_id|TEXT|1|1", "depends_on_task_id|TEXT|1|2"}},
      {"artifact",
       {"artifact_id|TEXT|0|1", "task_id|TEXT|1|0", "relative_path|TEXT|1|0", "blake3|TEXT|1|0",
        "byte_length|INTEGER|1|0"}},
      {"artifact_provenance",
       {"artifact_id|TEXT|0|1", "processor_id|TEXT|1|0", "parameters_blake3|TEXT|1|0",
        "model_refs_json|TEXT|1|0"}},
      {"cache_hit",
       {"task_id|TEXT|0|1", "cache_key|TEXT|1|0", "artifact_id|TEXT|1|0",
        "verified|INTEGER|1|0"}},
      {"failure",
       {"id|INTEGER|0|1", "task_id|TEXT|0|0", "occurred_utc|TEXT|1|0", "reason|TEXT|1|0"}},
      {"worker_session",
       {"worker_session_id|TEXT|0|1", "worker_id|TEXT|1|0", "runtime_id|TEXT|0|0",
        "started_utc|TEXT|1|0", "ended_utc|TEXT|0|0", "status|TEXT|1|0"}},
      {"task_attempt",
       {"task_id|TEXT|1|1", "attempt|INTEGER|1|2", "worker_session_id|TEXT|0|0",
        "lease_id|TEXT|0|0", "leased_utc|TEXT|0|0", "started_utc|TEXT|0|0",
        "completed_utc|TEXT|0|0", "queue_ms|INTEGER|0|0", "transfer_ms|INTEGER|0|0",
        "compute_ms|INTEGER|0|0", "cpu_ms|INTEGER|0|0", "peak_rss_bytes|INTEGER|0|0",
        "outcome|TEXT|1|0", "error|TEXT|0|0"}},
      {"blob_location",
       {"blake3|TEXT|1|1", "location|TEXT|1|2", "byte_length|INTEGER|1|0",
        "recorded_utc|TEXT|1|0"}},
  };
  return schema;
}

void test_layout_and_manifest() {
  const JournalLayout layout = journal_layout_for("/renders/video.svp");
  expect(layout.root == fs::path("/renders/video.svp-journal"), "journal root is <output>-journal");
  expect(layout.database == layout.root / "build.sqlite", "database path");
  expect(layout.manifest == layout.root / "journal_manifest.json", "manifest path");
  expect(layout.pending == layout.root / "blobs" / "pending", "pending path");
  expect(layout.completed == layout.root / "blobs" / "completed", "completed path");
  expect(layout.lock_file == layout.root / "locks" / "build.lock", "lock path");

  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  const JournalLayout& created = journal.layout();
  for (const fs::path& directory : {created.pending, created.completed, created.locks}) {
    expect(fs::is_directory(directory), "created " + directory.string());
  }
  expect(fs::is_regular_file(created.database) && fs::is_regular_file(created.lock_file),
         "database and lock file exist");
  const nlohmann::json manifest = decode_canonical_json(read_file(created.manifest));
  expect(manifest.at("format") == "svp-recovery-journal-v1", "manifest format");
  expect(manifest.at("build_session_id") == "bs_0001", "manifest session");
  expect(manifest.at("database") == "build.sqlite", "manifest names the database");
  expect(journal.build_session_id() == "bs_0001", "journal exposes its session id");
}

void test_schema_and_wal() {
  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  JournalDatabase database(journal.layout().database);
  expect_equal(database.value("PRAGMA journal_mode"), "wal", "build.sqlite is in WAL mode");
  for (const auto& [table, columns] : expected_schema()) {
    const std::vector<std::string> actual = database.rows(
        "SELECT name || '|' || type || '|' || \"notnull\" || '|' || pk FROM pragma_table_info('" +
        table + "')");
    expect(actual == columns, "table " + table + " matches its definition");
  }
  const std::vector<std::string> tables =
      database.rows("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE "
                    "'sqlite_%' ORDER BY name");
  expect(tables.size() == expected_schema().size(), "no unexpected tables");

  expect_equal(database.value("SELECT id, status, svp_version FROM build_session"),
               "bs_0001\tactive\t1.0-rc2", "build session recorded");
  expect_equal(database.value("SELECT source_id, size_bytes, mtime_ns, blake3 FROM source_fingerprint"),
               "source_000\t" + std::to_string(fixture.source.size_bytes) + "\t1\t" +
                   blake3_hex(fixture.source.blake3),
               "source fingerprint recorded");
}

void test_exclusive_lock() {
  JournalFixture fixture(kSuite);
  {
    RecoveryJournal journal = fixture.create();
    expect_journal_error(JournalErrorCode::locked,
                         [&] { (void)RecoveryJournal::open(fixture.output_path); },
                         "second opener while the first holds the journal");
    expect_journal_error(JournalErrorCode::already_exists, [&] { (void)fixture.create(); },
                         "create over an existing journal");
    try {
      (void)RecoveryJournal::open(fixture.output_path);
    } catch (const JournalError& error) {
      expect(std::string(error.what()).find("pid ") != std::string::npos,
             "lock error names the holder");
    }
  }
  RecoveryJournal reopened = RecoveryJournal::open(fixture.output_path);
  expect(reopened.build_session_id() == "bs_0001", "open after the holder closed");

  expect_journal_error(JournalErrorCode::not_found,
                       [&] { (void)RecoveryJournal::open(fixture.scratch.path / "other.svp"); },
                       "open without a journal");
}

void test_failed_create_leaves_nothing() {
  JournalFixture fixture(kSuite);
  BuildSessionRecord bad = sample_session();
  bad.id = "not an id";
  expect_journal_error(
      JournalErrorCode::invalid_argument,
      [&] { (void)RecoveryJournal::create(fixture.output_path, bad, fixture.sources()); },
      "invalid session id");
  expect(!fs::exists(journal_layout_for(fixture.output_path).root),
         "a failed create removes its directory");
}

void test_state_transitions() {
  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  journal.record_task(task_record("task.ocr.batch_000"));
  expect(journal.task_state("task.ocr.batch_000") == TaskState::planned, "recorded as planned");
  expect(!journal.task_state("task.unknown").has_value(), "unknown task has no state");

  expect_journal_error(JournalErrorCode::invalid_transition,
                       [&] { journal.set_task_state("task.ocr.batch_000", TaskState::running); },
                       "planned -> running skips ready");
  for (const TaskState next : {TaskState::ready, TaskState::leased, TaskState::ready,
                               TaskState::leased, TaskState::running}) {
    journal.set_task_state("task.ocr.batch_000", next);
    expect(journal.task_state("task.ocr.batch_000") == next,
           "entered " + std::string(task_state_name(next)));
  }
  expect_journal_error(
      JournalErrorCode::invalid_transition,
      [&] { journal.set_task_state("task.ocr.batch_000", TaskState::committed); },
      "committed only through commit_task");
  journal.set_task_state("task.ocr.batch_000", TaskState::failed_retryable);
  journal.set_task_state("task.ocr.batch_000", TaskState::ready);
  expect_journal_error(JournalErrorCode::unknown_task,
                       [&] { journal.set_task_state("task.unknown", TaskState::ready); },
                       "transition of an unknown task");
  expect_journal_error(JournalErrorCode::duplicate_record,
                       [&] { journal.record_task(task_record("task.ocr.batch_000")); },
                       "duplicate task");
  expect_journal_error(JournalErrorCode::invalid_argument,
                       [&] { journal.record_task(task_record("task with spaces")); },
                       "task id rule");

  JournalDatabase database(journal.layout().database);
  expect_equal(database.value("SELECT status, started_utc, cache_key FROM task"),
               "ready\t2026-09-30T12:34:56.789Z\t" + blake3_prefixed(blake3_digest("cache key")),
               "entering running stamps started_utc; cache_key uses the b3: form");
}

void test_commit_path() {
  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  journal.record_task(task_record("task.ocr.batch_000"));
  const fs::path file_output = fixture.scratch.path / "detections.jsonl";
  const std::string file_content = "{\"box\":1}\n{\"box\":2}\n";
  write_file(file_output, file_content);

  run_and_commit(journal, "task.ocr.batch_000",
                 {bytes_artifact("artifact_text_000", "visible text"),
                  JournalArtifactInput{.artifact_id = "artifact_boxes_000",
                                       .blake3 = blake3_digest(file_content),
                                       .content = file_output,
                                       .provenance = sample_provenance()}});

  expect(journal.task_state("task.ocr.batch_000") == TaskState::committed, "task committed");
  const fs::path text_blob = journal.layout().completed / blake3_hex(blake3_digest("visible text"));
  const fs::path file_blob = journal.layout().completed / blake3_hex(blake3_digest(file_content));
  expect_equal(read_file(text_blob), "visible text", "bytes artifact in blobs/completed");
  expect_equal(read_file(file_blob), file_content, "file artifact in blobs/completed");
  expect(count_entries(journal.layout().pending) == 0, "pending is empty after commit");

  JournalDatabase database(journal.layout().database);
  expect_equal(
      database.value("SELECT relative_path, blake3, byte_length FROM artifact "
                     "WHERE artifact_id = 'artifact_text_000'"),
      "blobs/completed/" + blake3_hex(blake3_digest("visible text")) + "\t" +
          blake3_hex(blake3_digest("visible text")) + "\t12",
      "artifact row");
  expect_equal(database.value("SELECT processor_id, model_refs_json FROM artifact_provenance "
                              "WHERE artifact_id = 'artifact_text_000'"),
               "proc_ocr_0001\t[\"model_ppocr_v5_det\",\"model_ppocr_v5_rec\"]",
               "provenance row");
  expect_equal(database.value("SELECT output_blake3, completed_utc FROM task"),
               blake3_hex(blake3_digest("output of task.ocr.batch_000")) +
                   "\t2026-09-30T12:34:56.789Z",
               "task output digest and completion time");
}

void test_commit_rejects_mismatch() {
  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  journal.record_task(task_record("task.ocr.batch_000"));
  journal.set_task_state("task.ocr.batch_000", TaskState::ready);
  journal.set_task_state("task.ocr.batch_000", TaskState::running);
  journal.set_task_state("task.ocr.batch_000", TaskState::result_received);

  JournalArtifactInput good = bytes_artifact("artifact_good", "good bytes");
  JournalArtifactInput lying = bytes_artifact("artifact_bad", "actual bytes");
  lying.blake3 = blake3_digest("declared bytes");
  expect_journal_error(JournalErrorCode::artifact_mismatch,
                       [&] {
                         journal.commit_task(TaskCommit{.task_id = "task.ocr.batch_000",
                                                        .output_blake3 = blake3_digest("o"),
                                                        .artifacts = {good, lying}});
                       },
                       "declared digest must match the bytes");
  expect(journal.task_state("task.ocr.batch_000") == TaskState::result_received,
         "a rejected commit leaves the task untouched");
  expect(count_entries(journal.layout().completed) == 0, "no blob published on rejection");
  expect(count_entries(journal.layout().pending) == 0, "staged blobs discarded on rejection");
  JournalDatabase database(journal.layout().database);
  expect_equal(database.value("SELECT count(*) FROM artifact"), "0", "no artifact rows");

  expect_journal_error(JournalErrorCode::invalid_argument,
                       [&] {
                         journal.commit_task(TaskCommit{
                             .task_id = "task.ocr.batch_000",
                             .output_blake3 = blake3_digest("o"),
                             .artifacts = {good},
                             .cache_hit = CacheHitRecord{.cache_key = blake3_digest("k"),
                                                         .artifact_id = "artifact_other"}});
                       },
                       "cache hit must reference a committed artifact");
}

void test_cache_hit_commit() {
  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  journal.record_task(task_record("task.ocr.batch_000"));
  journal.set_task_state("task.ocr.batch_000", TaskState::ready);
  journal.set_task_state("task.ocr.batch_000", TaskState::result_received);
  journal.commit_task(TaskCommit{
      .task_id = "task.ocr.batch_000",
      .output_blake3 = blake3_digest("o"),
      .artifacts = {bytes_artifact("artifact_cached", "cached bytes")},
      .cache_hit = CacheHitRecord{.cache_key = blake3_digest("cache key"),
                                  .artifact_id = "artifact_cached",
                                  .verified = true}});
  JournalDatabase database(journal.layout().database);
  expect_equal(database.value("SELECT cache_key, artifact_id, verified FROM cache_hit"),
               blake3_prefixed(blake3_digest("cache key")) + "\tartifact_cached\t1",
               "cache hit recorded (RC2 §20.3)");
}

void test_failures_and_distributed_records() {
  JournalFixture fixture(kSuite);
  RecoveryJournal journal = fixture.create();
  journal.record_failure(std::string("task.ocr.batch_000"), "worker lost");
  journal.record_failure(std::nullopt, "disk full");
  journal.record_worker_session(WorkerSessionRecord{.worker_session_id = "ws_0001",
                                                    .worker_id = "worker_a",
                                                    .runtime_id = blake3_digest("runtime"),
                                                    .started_utc = "2026-09-30T12:00:01.000Z"});
  TaskAttemptRecord attempt{.task_id = "task.ocr.batch_000",
                            .attempt = 1,
                            .worker_session_id = "ws_0001",
                            .lease_id = "lease_0001",
                            .leased_utc = "2026-09-30T12:00:02.000Z",
                            .outcome = AttemptOutcome::leased};
  journal.record_task_attempt(attempt);
  attempt.outcome = AttemptOutcome::succeeded;
  attempt.queue_ms = 3;
  attempt.transfer_ms = 40;
  attempt.compute_ms = 9850;
  attempt.cpu_ms = 52046;
  attempt.peak_rss_bytes = 1221541888;
  journal.record_task_attempt(attempt);
  journal.record_blob_location(BlobLocationRecord{.blake3 = blake3_digest("blob"),
                                                  .location = std::string(kJournalBlobLocation),
                                                  .byte_length = 4});
  journal.set_build_session_status("bs_0001", BuildSessionStatus::interrupted);
  expect_journal_error(JournalErrorCode::invalid_argument,
                       [&] { journal.set_build_session_status("bs_9999", BuildSessionStatus::failed); },
                       "unknown build session");

  JournalDatabase database(journal.layout().database);
  expect_equal(database.value("SELECT count(*) FROM failure WHERE occurred_utc = "
                              "'2026-09-30T12:34:56.789Z'"),
               "2", "failures stamped by the journal clock");
  expect_equal(database.value("SELECT worker_id, status, runtime_id FROM worker_session"),
               "worker_a\tactive\t" + blake3_prefixed(blake3_digest("runtime")),
               "worker session row");
  expect_equal(database.value("SELECT count(*), outcome, compute_ms, peak_rss_bytes "
                              "FROM task_attempt"),
               "1\tsucceeded\t9850\t1221541888", "attempt re-recorded in place");
  expect_equal(database.value("SELECT location, byte_length FROM blob_location"), "journal\t4",
               "blob location row");
  expect_equal(database.value("SELECT status FROM build_session"), "interrupted",
               "session status updated");
}

void test_cleanup_on_success() {
  JournalFixture fixture(kSuite);
  {
    RecoveryJournal journal = fixture.create();
    journal.record_task(task_record("task.ocr.batch_000"));
    run_and_commit(journal, "task.ocr.batch_000", {bytes_artifact("artifact_000", "bytes")});
    const JournalFinish finish = journal.finish_success(JournalRetention::delete_on_success);
    expect(!finish.retained, "default finish does not retain");
    expect(!fs::exists(finish.journal_root), "RC2 §20.5.1: journal deleted after success");
    expect_journal_error(JournalErrorCode::closed, [&] { (void)journal.layout(); },
                         "journal is closed after finish");
  }
  {
    RecoveryJournal journal = fixture.create();
    const JournalFinish finish = journal.finish_success(JournalRetention::retain_for_diagnostics);
    expect(finish.retained && fs::exists(finish.journal_root),
           "diagnostic retention keeps the journal");
    JournalDatabase database(journal_layout_for(fixture.output_path).database);
    expect_equal(database.value("SELECT status FROM build_session"), "succeeded",
                 "retained journal records success");
  }
  {
    RecoveryJournal holder = RecoveryJournal::open(fixture.output_path);
    expect_journal_error(JournalErrorCode::locked, [&] { remove_journal(fixture.output_path); },
                         "remove_journal refuses a live journal");
  }
  remove_journal(fixture.output_path);
  expect(!fs::exists(journal_layout_for(fixture.output_path).root), "remove_journal deletes");
  remove_journal(fixture.output_path);  // missing journal is not an error
}

}  // namespace

int main() {
  return run_tests(kSuite, {
                               {"layout_and_manifest", test_layout_and_manifest},
                               {"schema_and_wal", test_schema_and_wal},
                               {"exclusive_lock", test_exclusive_lock},
                               {"failed_create_leaves_nothing", test_failed_create_leaves_nothing},
                               {"state_transitions", test_state_transitions},
                               {"commit_path", test_commit_path},
                               {"commit_rejects_mismatch", test_commit_rejects_mismatch},
                               {"cache_hit_commit", test_cache_hit_commit},
                               {"failures_and_distributed_records",
                                test_failures_and_distributed_records},
                               {"cleanup_on_success", test_cleanup_on_success},
                           });
}
