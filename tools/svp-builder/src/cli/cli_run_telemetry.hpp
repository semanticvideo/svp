#pragma once

#include "svp/builder/build_progress.hpp"
#include "svp/builder/build_run_report.hpp"
#include "svp/builder/progress_timeline.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// Per-command progress timeline plus the optional --run-report writer.
// The timeline origin is the moment this object is constructed, which is the
// start of the command. When a report path is given, the report is written by
// finish(); if the command unwinds by exception first, the destructor writes a
// failed report instead.
class CliRunTelemetry {
 public:
  CliRunTelemetry(std::string command,
                  std::shared_ptr<svp::builder::BuildProgressSink> render_sink,
                  std::filesystem::path run_report_path);
  ~CliRunTelemetry();

  CliRunTelemetry(const CliRunTelemetry&) = delete;
  CliRunTelemetry& operator=(const CliRunTelemetry&) = delete;

  [[nodiscard]] std::shared_ptr<svp::builder::BuildProgressSink>
  progress_sink() const;

  // Records the build's resolved thread plan in the run report.
  void record_thread_plan(
      std::optional<svp::models::ThreadPlanResolution> thread_plan);

  // Writes the run report (if requested) and returns the command's exit code:
  // `exit_code` unchanged, except that a successful command whose requested
  // report could not be written fails with kBuildFailedExitCode.
  [[nodiscard]] int finish(int exit_code);

 private:
  // Returns false only when a requested report could not be written.
  bool write_report(std::optional<int> exit_code) noexcept;

  std::string command_;
  std::filesystem::path run_report_path_;
  std::shared_ptr<svp::builder::BuildRunReportRecorder> recorder_;
  std::shared_ptr<svp::builder::TimestampedProgressSink> timeline_;
  std::optional<svp::models::ThreadPlanResolution> thread_plan_;
  bool finished_ = false;
};
