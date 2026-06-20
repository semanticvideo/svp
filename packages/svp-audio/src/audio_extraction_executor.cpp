#include "svp/audio/audio_extraction_executor.hpp"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace svp::audio {
namespace {

bool is_safe_output_ref(const std::string& output_ref) {
  if (output_ref.empty()) {
    return false;
  }

  const std::filesystem::path path(output_ref);
  if (path.is_absolute()) {
    return false;
  }

  for (const auto& component : path) {
    if (component == "..") {
      return false;
    }
  }

  return true;
}

std::filesystem::path staged_path_for_ref(const std::filesystem::path& staging_root,
                                          const std::string& output_ref) {
  return staging_root / std::filesystem::path(output_ref);
}

std::vector<std::string> rewrite_output_ref_arguments(
    const std::vector<std::string>& arguments,
    const std::string& output_ref,
    const std::filesystem::path& staged_output_path,
    bool& output_ref_found) {
  std::vector<std::string> rewritten;
  rewritten.reserve(arguments.size());
  output_ref_found = false;

  for (const std::string& argument : arguments) {
    if (argument == output_ref) {
      rewritten.push_back(staged_output_path.string());
      output_ref_found = true;
    } else {
      rewritten.push_back(argument);
    }
  }

  return rewritten;
}

int run_process(const std::vector<std::string>& arguments) {
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
  }

  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  pid_t waited = 0;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);

  if (waited < 0) {
    throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
  }

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

AudioExtractionCommandRun execute_command(const std::string& task_id,
                                          const std::string& output_ref,
                                          const std::vector<std::string>& arguments,
                                          const std::filesystem::path& staging_root) {
  AudioExtractionCommandRun run;
  run.task_id = task_id;
  run.output_ref = output_ref;
  run.command_available = !arguments.empty();

  if (!run.command_available) {
    run.skipped_reason = "command plan is not available";
    return run;
  }

  if (!is_safe_output_ref(output_ref)) {
    run.skipped_reason = "output_ref is not a safe relative package path";
    return run;
  }

  run.staged_output_path = staged_path_for_ref(staging_root, output_ref);
  bool output_ref_found = false;
  run.arguments = rewrite_output_ref_arguments(arguments,
                                               output_ref,
                                               run.staged_output_path,
                                               output_ref_found);
  if (!output_ref_found) {
    run.skipped_reason = "command arguments do not contain the planned output_ref";
    return run;
  }

  run.command_safe = true;
  std::filesystem::create_directories(run.staged_output_path.parent_path());
  const int exit_code = run_process(run.arguments);
  run.command_executed = true;
  run.exit_code = exit_code;
  run.success = exit_code == 0 && std::filesystem::exists(run.staged_output_path);
  if (!run.success) {
    run.skipped_reason = "command executed but did not produce the planned output";
  }

  return run;
}

nlohmann::json command_run_to_json(const AudioExtractionCommandRun& run) {
  nlohmann::json encoded = {
      {"task_id", run.task_id},
      {"output_ref", run.output_ref},
      {"staged_output_path", run.staged_output_path.string()},
      {"arguments", run.arguments},
      {"command_available", run.command_available},
      {"command_safe", run.command_safe},
      {"command_executed", run.command_executed},
      {"success", run.success},
  };

  if (run.exit_code.has_value()) {
    encoded["exit_code"] = *run.exit_code;
  }
  if (!run.skipped_reason.empty()) {
    encoded["skipped_reason"] = run.skipped_reason;
  }

  return encoded;
}

}  // namespace

AudioExtractionRun execute_audio_extraction_plan(const AudioExtractionPlan& plan,
                                                 const std::filesystem::path& staging_root) {
  AudioExtractionRun run;
  run.staging_root = staging_root;
  std::filesystem::create_directories(staging_root);

  for (const AudioExtractionCommandPlan& command : plan.original_streams) {
    AudioExtractionCommandRun command_run =
        execute_command(command.task_id, command.output_ref, command.arguments, staging_root);
    run.extraction_run = run.extraction_run || command_run.command_executed;
    if (!command_run.success && !command_run.skipped_reason.empty()) {
      run.blockers.push_back(command.task_id + ": " + command_run.skipped_reason);
    }
    run.original_streams.push_back(std::move(command_run));
  }

  run.analysis_audio = execute_command(plan.analysis_audio.task_id,
                                       plan.analysis_audio.output_ref,
                                       plan.analysis_audio.arguments,
                                       staging_root);
  run.extraction_run = run.extraction_run || run.analysis_audio.command_executed;
  if (!run.analysis_audio.success && !run.analysis_audio.skipped_reason.empty()) {
    run.blockers.push_back(plan.analysis_audio.task_id + ": " +
                           run.analysis_audio.skipped_reason);
  }

  run.original_streams_written = !run.original_streams.empty();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    run.original_streams_written = run.original_streams_written && command_run.success;
  }
  run.analysis_audio_written = run.analysis_audio.success;

  return run;
}

nlohmann::json audio_extraction_run_to_json(const AudioExtractionRun& run) {
  nlohmann::json original_streams = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    original_streams.push_back(command_run_to_json(command_run));
  }

  return {
      {"staging_root", run.staging_root.string()},
      {"original_streams", original_streams},
      {"analysis_audio", command_run_to_json(run.analysis_audio)},
      {"blockers", run.blockers},
      {"extraction_run", run.extraction_run},
      {"original_streams_written", run.original_streams_written},
      {"analysis_audio_written", run.analysis_audio_written},
  };
}

}  // namespace svp::audio
