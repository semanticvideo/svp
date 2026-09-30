// RC2 §20.5.1 journal cleanup.

#include "svp/exec/journal_cleanup.hpp"

#include "file_lock.hpp"
#include "recovery_journal_state.hpp"
#include "svp/exec/journal_error.hpp"
#include "svp/exec/journal_layout.hpp"

namespace svp::exec {
namespace {

namespace fs = std::filesystem;

void remove_tree(const fs::path& root) {
  std::error_code error;
  fs::remove_all(root, error);
  if (error) {
    throw JournalError(JournalErrorCode::io_error,
                       "remove " + root.string() + ": " + error.message());
  }
}

}  // namespace

JournalFinish RecoveryJournal::finish_success(JournalRetention retention) {
  detail::JournalState& journal = state();
  set_build_session_status(journal.manifest.build_session_id, BuildSessionStatus::succeeded);
  JournalFinish finish{.retained = retention == JournalRetention::retain_for_diagnostics,
                       .journal_root = journal.layout.root};
  if (finish.retained) {
    close();
    return finish;
  }
  // Delete while still holding build.lock so no other build can open the
  // journal half-removed; the lock dies with its (unlinked) file.
  journal.database.close();
  remove_tree(journal.layout.root);
  journal.lock.release();
  return finish;
}

void remove_journal(const fs::path& output_path) {
  const JournalLayout layout = journal_layout_for(output_path);
  std::error_code error;
  if (!fs::exists(layout.root, error)) {
    return;
  }
  // Only lock when the locks directory exists; creating it would re-create
  // part of the tree being removed.
  detail::FileLock lock;
  if (fs::is_directory(layout.locks, error)) {
    detail::LockAttempt attempt =
        detail::lock_file(layout.lock_file, detail::LockMode::exclusive, detail::LockWait::try_once);
    if (attempt.outcome == detail::LockOutcome::contended) {
      throw JournalError(JournalErrorCode::locked,
                         "recovery journal " + layout.root.string() + " is in use by another build");
    }
    if (attempt.outcome == detail::LockOutcome::failed) {
      throw JournalError(JournalErrorCode::io_error,
                         "lock " + layout.lock_file.string() + ": " + attempt.error.message());
    }
    lock = std::move(attempt.lock);
  }
  remove_tree(layout.root);
}

}  // namespace svp::exec
