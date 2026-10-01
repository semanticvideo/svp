#include "engine/build_journal_session.hpp"

#include "svp/core/version.hpp"
#include "svp/exec/exec_error.hpp"
#include "svp/exec/journal_error.hpp"
#include "svp/exec/journal_layout.hpp"
#include "svp/exec/journal_scheduler_resume.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

namespace svp::builder::engine {
namespace fs = std::filesystem;

namespace {

// Random bits in a build session ID: it only has to be unique among the
// journals of one output path, which 128 bits are by a wide margin.
constexpr std::size_t kSessionIdRandomWords = 4;

std::string new_build_session_id() {
  std::random_device random;
  std::ostringstream id;
  id << "bs_" << std::hex << std::setfill('0');
  for (std::size_t word = 0; word < kSessionIdRandomWords; ++word) {
    id << std::setw(8) << static_cast<std::uint32_t>(random());
  }
  return id.str();
}

std::string utc_now() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::ostringstream out;
  out << std::put_time(std::gmtime(&now), "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string fresh_hint() { return "pass --fresh to discard it and start over"; }

}  // namespace

BuildJournalSession::BuildJournalSession(RecoveryJournalMode mode,
                                         fs::path journal_output_path)
    : mode_(mode),
      journal_output_path_(std::move(journal_output_path)),
      journal_root_(svp::exec::journal_layout_for(journal_output_path_).root) {}

std::string BuildJournalSession::prepare() {
  std::error_code error;
  const bool exists = fs::exists(journal_root_, error);
  switch (mode_) {
    case RecoveryJournalMode::require_new:
      if (exists) {
        throw RecoveryJournalBlocked(
            "a recovery journal from an earlier interrupted or failed build of " +
            journal_output_path_.string() + " exists at " + journal_root_.string() +
            "; pass --resume to continue that build, or " + fresh_hint());
      }
      session_id_ = new_build_session_id();
      return session_id_;
    case RecoveryJournalMode::fresh:
      if (exists) {
        try {
          svp::exec::remove_journal(journal_output_path_);
        } catch (const svp::exec::JournalError& journal_error) {
          throw RecoveryJournalBlocked("--fresh: cannot remove the recovery journal at " +
                                       journal_root_.string() + ": " +
                                       journal_error.what());
        }
      }
      session_id_ = new_build_session_id();
      return session_id_;
    case RecoveryJournalMode::resume:
      if (!exists) {
        throw RecoveryJournalBlocked("--resume: there is no recovery journal at " +
                                     journal_root_.string() +
                                     " to resume; run without --resume to start a new build");
      }
      try {
        opened_.emplace(svp::exec::RecoveryJournal::open(journal_output_path_));
      } catch (const svp::exec::JournalError& journal_error) {
        throw RecoveryJournalBlocked("--resume: cannot open the recovery journal at " +
                                     journal_root_.string() + ": " + journal_error.what() +
                                     "; " + fresh_hint());
      }
      session_id_ = opened_->build_session_id();
      return session_id_;
  }
  throw std::logic_error("unknown recovery journal mode");
}

StartedJournal BuildJournalSession::start(const svp::exec::TaskGraph& graph,
                                          const svp::exec::SourceFingerprintRecord& source) {
  const std::array<svp::exec::SourceFingerprintRecord, 1> sources{source};
  if (mode_ != RecoveryJournalMode::resume) {
    const svp::exec::BuildSessionRecord session{
        .id = session_id_,
        .started_utc = utc_now(),
        .svp_version = svp::core::spec_version_label(),
        .builder_version = svp::core::tool_version_label("svp-builder"),
        .status = svp::exec::BuildSessionStatus::active};
    // Outputs get their missing parent directories created (output path
    // policy); the journal beside the output follows the same rule.
    std::error_code parent_error;
    if (!journal_root_.parent_path().empty()) {
      fs::create_directories(journal_root_.parent_path(), parent_error);
    }
    StartedJournal started{
        .journal = svp::exec::RecoveryJournal::create(journal_output_path_, session, sources),
        .committed = {},
        .resume_report = std::nullopt};
    svp::exec::record_task_graph(started.journal, graph);
    return started;
  }

  svp::exec::RecoveryJournal journal = std::move(*opened_);
  opened_.reset();
  try {
    svp::exec::SchedulerResumeState state =
        svp::exec::resume_scheduler_state(journal, graph, sources);
    return StartedJournal{.journal = std::move(journal),
                          .committed = std::move(state.committed),
                          .resume_report = std::move(state.report)};
  } catch (const svp::exec::JournalError& journal_error) {
    if (journal_error.code() == svp::exec::JournalErrorCode::source_mismatch) {
      throw RecoveryJournalBlocked(
          "--resume: the source media no longer matches the recovery journal (" +
          std::string(journal_error.what()) + "); " + fresh_hint());
    }
    throw;
  } catch (const svp::exec::ExecError& exec_error) {
    if (exec_error.code() == svp::exec::ExecErrorCode::invalid_value) {
      throw RecoveryJournalBlocked(
          "--resume: the recovery journal was recorded for different build inputs or "
          "options (" + std::string(exec_error.what()) +
          "); rerun with the original options, or " + fresh_hint());
    }
    throw;
  }
}

}  // namespace svp::builder::engine
