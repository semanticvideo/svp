#pragma once

// Shared fixtures for the recovery-journal tests.

#include "storage_test_support.hpp"
#include "svp/exec/recovery_journal.hpp"

#include <sqlite3.h>

namespace svp::exec::test {

inline BuildSessionRecord sample_session() {
  return BuildSessionRecord{.id = "bs_0001",
                            .started_utc = "2026-09-30T12:00:00.000Z",
                            .svp_version = "1.0-rc2",
                            .builder_version = "svp-builder 1.0.0-rc2",
                            .status = BuildSessionStatus::active};
}

// A deterministic clock so journal-stamped columns can be asserted.
inline JournalOptions fixed_clock_options() {
  return JournalOptions{.utc_now = [] { return std::string("2026-09-30T12:34:56.789Z"); }};
}

struct JournalFixture {
  TemporaryDirectory scratch;
  fs::path source_path;
  fs::path output_path;
  SourceFingerprintRecord source;

  explicit JournalFixture(std::string_view suite) : scratch(suite) {
    source_path = scratch.path / "input.mov";
    write_file(source_path, "pretend this is a QuickTime movie");
    output_path = scratch.path / "input.svp";
    source = SourceFingerprintRecord{.source_id = "source_000",
                                     .path = source_path.string(),
                                     .size_bytes = fs::file_size(source_path),
                                     .mtime_ns = 1,
                                     .blake3 = blake3_digest(read_file(source_path))};
  }

  [[nodiscard]] std::span<const SourceFingerprintRecord> sources() const {
    return std::span(&source, 1);
  }

  [[nodiscard]] RecoveryJournal create() const {
    return RecoveryJournal::create(output_path, sample_session(), sources(),
                                   fixed_clock_options());
  }
};

inline JournalTaskRecord task_record(std::string task_id,
                                     std::vector<std::string> depends_on = {}) {
  return JournalTaskRecord{.task_id = std::move(task_id),
                           .task_type = "ocr.frame_batch",
                           .cache_key = blake3_digest("cache key"),
                           .depends_on = std::move(depends_on)};
}

inline ArtifactProvenanceRecord sample_provenance() {
  return ArtifactProvenanceRecord{.processor_id = "proc_ocr_0001",
                                  .parameters_blake3 = blake3_digest("parameters"),
                                  .model_refs = {"model_ppocr_v5_det", "model_ppocr_v5_rec"}};
}

inline JournalArtifactInput bytes_artifact(std::string artifact_id, std::string_view content) {
  return JournalArtifactInput{.artifact_id = std::move(artifact_id),
                              .blake3 = blake3_digest(content),
                              .content = as_byte_span(content),
                              .provenance = sample_provenance()};
}

// Drives a recorded task to result_received and commits `artifacts`.
inline void run_and_commit(RecoveryJournal& journal, const std::string& task_id,
                           std::vector<JournalArtifactInput> artifacts) {
  journal.set_task_state(task_id, TaskState::ready);
  journal.set_task_state(task_id, TaskState::running);
  journal.set_task_state(task_id, TaskState::result_received);
  journal.commit_task(TaskCommit{.task_id = task_id,
                                 .output_blake3 = blake3_digest("output of " + task_id),
                                 .artifacts = std::move(artifacts)});
}

// Read-only view of build.sqlite for assertions.
class JournalDatabase {
 public:
  explicit JournalDatabase(const fs::path& path) {
    if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      throw std::runtime_error("cannot open " + path.string());
    }
  }
  ~JournalDatabase() { sqlite3_close_v2(database_); }
  JournalDatabase(const JournalDatabase&) = delete;
  JournalDatabase& operator=(const JournalDatabase&) = delete;

  // Rows of a query as tab-separated text, NULL spelled "NULL".
  std::vector<std::string> rows(std::string_view sql) const {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database_, std::string(sql).c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
      throw std::runtime_error("bad query: " + std::string(sql) + ": " +
                               sqlite3_errmsg(database_));
    }
    std::vector<std::string> result;
    while (sqlite3_step(statement) == SQLITE_ROW) {
      std::string row;
      for (int column = 0; column < sqlite3_column_count(statement); ++column) {
        if (column != 0) {
          row += '\t';
        }
        const auto* text = sqlite3_column_text(statement, column);
        row += text == nullptr ? "NULL" : reinterpret_cast<const char*>(text);
      }
      result.push_back(std::move(row));
    }
    sqlite3_finalize(statement);
    return result;
  }

  std::string value(std::string_view sql) const {
    const auto result = rows(sql);
    return result.empty() ? std::string("<no row>") : result.front();
  }

 private:
  sqlite3* database_ = nullptr;
};

}  // namespace svp::exec::test
