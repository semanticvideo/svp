#include "distributed_cli.hpp"

#if defined(__APPLE__)
#include "workers/distributed_fleet.hpp"
#endif

#include <iostream>
#include <string>

void add_distributed_flags(CLI::App& command, bool& distributed, std::size_t& require_workers) {
  command.add_flag("--distributed", distributed,
                   "Also run this build's work on this Mac's paired workers "
                   "(svp-builder workers pair); unreachable workers are skipped with a warning");
  command.add_option("--require-workers", require_workers,
                     "With --distributed: fail before any work unless at least this many "
                     "paired workers are ready (implies --distributed)");
}

std::optional<std::shared_ptr<svp::builder::DistributedExecution>>
make_cli_distributed_execution(bool distributed, std::size_t require_workers, bool quiet,
                               std::string_view command_name) {
  if (!distributed && require_workers == 0) {
    return std::shared_ptr<svp::builder::DistributedExecution>{};
  }
#if defined(__APPLE__)
  return std::shared_ptr<svp::builder::DistributedExecution>(
      std::make_shared<svp::builder::workers::PairedWorkerFleet>(
          svp::builder::workers::DistributedFleetOptions{.require_workers = require_workers,
                                                         .quiet = quiet}));
#else
  (void)quiet;
  std::cerr << "svp-builder " << command_name << ": --distributed needs macOS (paired workers)\n";
  return std::nullopt;
#endif
}
