#include "cli_runtime_tools.hpp"

#include "cli_context.hpp"

#include "svp/core/executable_path.hpp"

#include <exception>
#include <filesystem>
#include <optional>

namespace {

using svp::builder::RuntimeTool;

std::string option_description(RuntimeTool tool) {
  const svp::builder::RuntimeToolSpec& spec = svp::builder::runtime_tool_spec(tool);
  const std::string environment = "$" + std::string(spec.environment_variable);
  if (tool == RuntimeTool::sherpa_onnx) {
    return "Path to libsherpa-onnx-c-api.dylib for diarization (default: " +
           environment +
           ", then the installed SVP runtime bundle, then pip/Homebrew locations)";
  }
  return std::string(spec.name) + " executable path (default: " + environment +
         ", then the installed SVP runtime bundle, then " + std::string(spec.name) +
         " on PATH)";
}

std::filesystem::path current_executable() {
  try {
    return svp::core::resolve_current_executable();
  } catch (const std::exception&) {
    // Without a resolvable executable there is no bundle to find; the
    // PATH/legacy defaults apply, as before bundles existed.
    return {};
  }
}

svp::builder::RuntimeToolSelection resolve_selected_command_tools(
    const std::vector<RuntimeToolBinding>& bindings) {
  svp::builder::RuntimeToolSelection selection;
  bool command_uses_tools = false;
  for (const RuntimeToolBinding& binding : bindings) {
    command_uses_tools = command_uses_tools || binding.command->parsed();
  }
  if (!command_uses_tools) return selection;

  const std::optional<svp::builder::RuntimeBundle> bundle =
      svp::builder::locate_runtime_bundle(current_executable());
  if (bundle) {
    selection.bundle_root = bundle->root;
    selection.runtime_id = bundle->runtime_id;
  }
  const svp::builder::EnvironmentLookup environment =
      svp::builder::process_environment();

  for (const RuntimeToolBinding& binding : bindings) {
    if (!binding.command->parsed()) continue;
    if (binding.tool == RuntimeTool::sherpa_onnx) {
      // svp-audio owns the sherpa search; the bundle adds one candidate after
      // the explicit path and SHERPA_ONNX_LIB_PATH.
      selection.uses_sherpa = true;
      selection.sherpa_bundled = svp::builder::offer_bundled_sherpa_library(bundle);
      continue;
    }
    const std::optional<std::string> flag_value =
        binding.option->count() > 0 ? std::optional<std::string>(*binding.target)
                                    : std::nullopt;
    svp::builder::RuntimeToolChoice choice = svp::builder::resolve_runtime_tool(
        binding.tool, flag_value, bundle, environment);
    *binding.target = choice.path;
    if (binding.tool == RuntimeTool::ffmpeg) {
      selection.ffmpeg = std::move(choice);
    } else {
      selection.ffprobe = std::move(choice);
    }
  }
  return selection;
}

}  // namespace

void add_runtime_tool_option(std::vector<RuntimeToolBinding>& bindings,
                             CLI::App& command, const std::string& flag,
                             std::string& target, RuntimeTool tool) {
  CLI::Option* option = command.add_option(flag, target, option_description(tool));
  bindings.push_back(RuntimeToolBinding{
      .command = &command, .option = option, .target = &target, .tool = tool});
}

void apply_cli_runtime_tools(CliContext& context) {
  const svp::builder::RuntimeToolSelection selection =
      resolve_selected_command_tools(context.runtime_tool_bindings);
  context.build_opts.runtime_tools = selection;
  context.build_batch_opts.runtime_tools = selection;
  context.interlace_opts.ic_runtime_tools = selection;
}
