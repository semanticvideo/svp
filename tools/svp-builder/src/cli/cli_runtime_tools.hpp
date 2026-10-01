#pragma once

#include "svp/builder/runtime_tools.hpp"

#include <CLI/CLI.hpp>

#include <string>
#include <vector>

struct CliContext;

// One --ffmpeg, --ffprobe, or --sherpa-lib option and the variable it fills.
struct RuntimeToolBinding {
  CLI::App* command = nullptr;
  CLI::Option* option = nullptr;
  std::string* target = nullptr;
  svp::builder::RuntimeTool tool = svp::builder::RuntimeTool::ffmpeg;
};

// Registers the tool's flag on `command` with a description of the default
// search order, and remembers it for apply_cli_runtime_tools().
void add_runtime_tool_option(std::vector<RuntimeToolBinding>& bindings,
                             CLI::App& command, const std::string& flag,
                             std::string& target, svp::builder::RuntimeTool tool);

// Runs after parsing, before the selected command. For the selected command:
// fills every tool flag the user did not pass from the environment override,
// the installed runtime bundle, or the PATH default (runtime_tools.hpp), and
// offers the bundle's sherpa-onnx library to svp-audio. Stores what was chosen
// in the command options that report it. Commands without tool flags are left
// untouched and never look for a bundle.
void apply_cli_runtime_tools(CliContext& context);
