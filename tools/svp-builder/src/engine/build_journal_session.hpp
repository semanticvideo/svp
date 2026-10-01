#pragma once

// Opens or creates the RC2 §20.4 recovery journal of one build according to
// its RecoveryJournalMode, in two steps because the task graph carries the
// journal's build session ID:
//
//   prepare()  require_new: fails if a journal exists (never reused or deleted
//                           silently), then picks a new session ID;
//              fresh:       deletes an existing journal, then picks a new ID;
//              resume:      opens the existing journal and returns its ID.
//   start()    new journal: creates it with the source fingerprint and records
//              the graph; resume: verifies the source fingerprint and every
//              completed artifact, checks the journal describes this exact
//              graph (same tasks, dependencies, and cache keys, so different
//              inputs or options are refused), and returns the verified
//              committed results.

#include "svp/builder/build_pipeline.hpp"
#include "svp/exec/journal_records.hpp"
#include "svp/exec/journal_resume.hpp"
#include "svp/exec/recovery_journal.hpp"
#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/task_graph.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::builder::engine {

// A journal condition that stops the build before any task runs; the message
// tells the user what to do (BuildPipelineFailure::recovery_journal).
class RecoveryJournalBlocked : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct StartedJournal {
  svp::exec::RecoveryJournal journal;
  // Resume only: verified results committed by the interrupted build, in
  // graph order.
  std::vector<svp::exec::CommittedResult> committed;
  std::optional<svp::exec::ResumeReport> resume_report;
};

class BuildJournalSession {
 public:
  BuildJournalSession(RecoveryJournalMode mode, std::filesystem::path journal_output_path);

  // Throws RecoveryJournalBlocked.
  [[nodiscard]] std::string prepare();

  // Throws RecoveryJournalBlocked; other journal failures (I/O, database)
  // propagate.
  [[nodiscard]] StartedJournal start(const svp::exec::TaskGraph& graph,
                                     const svp::exec::SourceFingerprintRecord& source);

  // After prepare() in resume mode: the IDs of every task the journal
  // recorded. Empty for a new journal.
  [[nodiscard]] std::vector<std::string> recorded_task_ids() const;

  [[nodiscard]] const std::filesystem::path& journal_root() const noexcept {
    return journal_root_;
  }

 private:
  RecoveryJournalMode mode_;
  std::filesystem::path journal_output_path_;
  std::filesystem::path journal_root_;
  std::string session_id_;
  std::optional<svp::exec::RecoveryJournal> opened_;
};

}  // namespace svp::builder::engine
