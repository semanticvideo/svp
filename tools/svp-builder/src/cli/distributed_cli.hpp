#pragma once

// --distributed / --require-workers for every command that builds a package
// (build, interlace create, interlace create-batch): the same flags and the
// same paired-worker fleet, so every build path can use this Mac's workers.

#include "svp/builder/distributed_execution.hpp"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

void add_distributed_flags(CLI::App& command, bool& distributed, std::size_t& require_workers);

// The build's paired workers when --distributed or --require-workers was
// given, null otherwise. nullopt (after printing why) when this platform has
// no paired workers; the command then exits 2.
[[nodiscard]] std::optional<std::shared_ptr<svp::builder::DistributedExecution>>
make_cli_distributed_execution(bool distributed, std::size_t require_workers, bool quiet,
                               std::string_view command_name);
