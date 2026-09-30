#include "cli_run_telemetry.hpp"

#include <exception>
#include <iostream>
#include <utility>
#include <vector>

CliRunTelemetry::CliRunTelemetry(
    std::string command,
    std::shared_ptr<svp::builder::BuildProgressSink> render_sink,
    std::filesystem::path run_report_path)
    : command_(std::move(command)),
      run_report_path_(std::move(run_report_path)) {
  std::vector<std::shared_ptr<svp::builder::BuildProgressSink>> downstream{
      std::move(render_sink)};
  if (!run_report_path_.empty()) {
    recorder_ = std::make_shared<svp::builder::BuildRunReportRecorder>();
    downstream.push_back(recorder_);
  }
  timeline_ = svp::builder::make_timestamped_progress_sink(std::move(downstream));
}

CliRunTelemetry::~CliRunTelemetry() {
  if (!finished_) write_report(std::nullopt);
}

std::shared_ptr<svp::builder::BuildProgressSink>
CliRunTelemetry::progress_sink() const {
  return timeline_;
}

int CliRunTelemetry::finish(int exit_code) {
  finished_ = true;
  write_report(exit_code);
  return exit_code;
}

void CliRunTelemetry::write_report(std::optional<int> exit_code) noexcept {
  if (!recorder_) return;
  try {
    svp::builder::BuildRunSummary summary{
        .command = command_,
        .exit_code = exit_code,
        .total_wall_ms = timeline_->elapsed_ms(),
        .event_count = timeline_->emitted_count(),
        .final_resources = svp::builder::sample_process_resources(),
    };
    svp::builder::write_build_run_report(run_report_path_, *recorder_, summary);
  } catch (const std::exception& error) {
    std::cerr << "svp-builder: " << error.what() << "\n";
  }
}
