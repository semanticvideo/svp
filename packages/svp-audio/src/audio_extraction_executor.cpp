#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/waveform_envelope.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
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

int run_process(const std::vector<std::string>& arguments,
                 bool suppress_stderr) {
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
    if (suppress_stderr) {
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }
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
                                          const std::filesystem::path& staging_root,
                                          bool suppress_stderr) {
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
  const int exit_code = run_process(run.arguments, suppress_stderr);
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

nlohmann::json derived_artifact_run_to_json(const AudioDerivedArtifactRun& run) {
  nlohmann::json encoded = {
      {"task_id", run.task_id},
      {"output_ref", run.output_ref},
      {"staged_output_path", run.staged_output_path.string()},
      {"written", run.written},
  };
  if (!run.skipped_reason.empty()) {
    encoded["skipped_reason"] = run.skipped_reason;
  }
  return encoded;
}

void write_json_file(const std::filesystem::path& path, const nlohmann::json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open staged audio JSON artifact: " + path.string());
  }
  output << value.dump(2) << "\n";
}

void write_jsonl_file(const std::filesystem::path& path,
                      const std::vector<nlohmann::json>& records) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("unable to open staged audio JSONL artifact: " + path.string());
  }
  for (const nlohmann::json& record : records) {
    output << record.dump() << "\n";
  }
}

nlohmann::json audio_absence_record(const AudioExtractionPlan& plan,
                                    const AudioExtractionRun& run) {
  nlohmann::json original_audio_refs = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    if (command_run.success) {
      original_audio_refs.push_back(command_run.output_ref);
    }
  }

  nlohmann::json microphone_analysis_refs = nlohmann::json::array();
  for (const auto& command_run : run.microphone_analysis_streams) {
    if (command_run.success) {
      microphone_analysis_refs.push_back(command_run.output_ref);
    }
  }

  return {
      {"source_audio_present", plan.source_audio_present},
      {"source_audio_stream_count", plan.original_streams.size()},
      {"selected_source_audio_stream_id",
       plan.analysis_audio.selected_source_audio_stream_id},
      {"canonical_silence_generated", false},
      {"original_audio_refs", original_audio_refs},
      {"microphone_analysis_refs", microphone_analysis_refs},
      {"analysis_audio_ref", run.analysis_audio.success ? run.analysis_audio.output_ref : ""},
      {"analysis_audio_written", run.analysis_audio.success},
      {"provenance_id", plan.audio_absence.processor_id},
      {"foundation_status", "staged_from_probe_and_audio_extraction_run"},
      {"final_package_ready", false},
      {"blockers", run.blockers},
  };
}

nlohmann::json extraction_processor_record(const AudioExtractionPlan& plan,
                                           const AudioExtractionRun& run) {
  nlohmann::json input_refs = nlohmann::json::array();
  input_refs.push_back(plan.source_path.string());

  nlohmann::json output_refs = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    output_refs.push_back(command_run.output_ref);
  }
  for (const AudioExtractionCommandRun& command_run : run.microphone_analysis_streams) {
    output_refs.push_back(command_run.output_ref);
  }
  output_refs.push_back(run.analysis_audio.output_ref);

  nlohmann::json task_ids = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    task_ids.push_back(command_run.task_id);
  }
  for (const AudioExtractionCommandRun& command_run : run.microphone_analysis_streams) {
    task_ids.push_back(command_run.task_id);
  }
  task_ids.push_back(run.analysis_audio.task_id);

  return {
      {"id", "proc_ffmpeg_audio_extraction_0001"},
      {"name", "FFmpeg audio extraction"},
      {"version", "pending_version_probe"},
      {"input_refs", input_refs},
      {"output_refs", output_refs},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", task_ids},
      {"runtime", "ffmpeg_cli"},
      {"execution_provider", "cpu"},
      {"foundation_status", run.extraction_run ? "attempted" : "planned"},
      {"completed", run.original_streams_written &&
                        run.microphone_analysis_streams_written &&
                        run.analysis_audio_written},
  };
}

nlohmann::json absence_processor_record(const AudioExtractionPlan& plan,
                                        const AudioExtractionRun& run) {
  nlohmann::json input_refs = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    if (command_run.success) {
      input_refs.push_back(command_run.output_ref);
    }
  }
  if (run.analysis_audio.success) {
    input_refs.push_back(run.analysis_audio.output_ref);
  }

  return {
      {"id", plan.audio_absence.processor_id},
      {"name", "SVP audio absence foundation writer"},
      {"version", "foundation"},
      {"input_refs", input_refs},
      {"output_refs", {plan.audio_absence.output_ref}},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {plan.audio_absence.task_id}},
      {"runtime", "svp-audio"},
      {"execution_provider", "cpu"},
      {"foundation_status", run.audio_absence_written ? "staged" : "planned"},
      {"completed", run.audio_absence_written},
  };
}

nlohmann::json waveform_processor_record(const AudioExtractionPlan& plan,
                                         const AudioExtractionRun& run) {
  return {
      {"id", plan.waveform.processor_id},
      {"name", "SVP waveform envelope"},
      {"version", "foundation"},
      {"input_refs", {plan.waveform.input_ref}},
      {"output_refs", {plan.waveform.output_ref}},
      {"model_refs", nlohmann::json::array()},
      {"task_ids", {plan.waveform.task_id}},
      {"runtime", "svp-audio"},
      {"execution_provider", "cpu"},
      {"window_duration_us", plan.waveform.window_duration_us},
      {"foundation_status", run.waveform_written ? "staged" : "planned"},
      {"completed", run.waveform_written},
  };
}

AudioDerivedArtifactRun stage_audio_absence(const AudioExtractionPlan& plan,
                                            AudioExtractionRun& run,
                                            const std::filesystem::path& staging_root) {
  AudioDerivedArtifactRun artifact;
  artifact.task_id = plan.audio_absence.task_id;
  artifact.output_ref = plan.audio_absence.output_ref;

  if (!is_safe_output_ref(artifact.output_ref)) {
    artifact.skipped_reason = "output_ref is not a safe relative package path";
    return artifact;
  }

  artifact.staged_output_path = staged_path_for_ref(staging_root, artifact.output_ref);
  write_json_file(artifact.staged_output_path, audio_absence_record(plan, run));
  artifact.written = true;
  return artifact;
}

AudioDerivedArtifactRun stage_processor_provenance(const AudioExtractionPlan& plan,
                                                   AudioExtractionRun& run,
                                                   const std::filesystem::path& staging_root) {
  AudioDerivedArtifactRun artifact;
  artifact.task_id = plan.processor_provenance.task_id;
  artifact.output_ref = plan.processor_provenance.output_ref;

  if (!is_safe_output_ref(artifact.output_ref)) {
    artifact.skipped_reason = "output_ref is not a safe relative package path";
    return artifact;
  }

  artifact.staged_output_path = staged_path_for_ref(staging_root, artifact.output_ref);
  write_jsonl_file(artifact.staged_output_path,
                   {extraction_processor_record(plan, run),
                    absence_processor_record(plan, run),
                    waveform_processor_record(plan, run)});
  artifact.written = true;
  return artifact;
}

AudioDerivedArtifactRun stage_waveform_artifact(const AudioExtractionPlan& plan,
                                                const AudioExtractionRun& run,
                                                const std::filesystem::path& staging_root) {
  AudioDerivedArtifactRun artifact;
  artifact.task_id = plan.waveform.task_id;
  artifact.output_ref = plan.waveform.output_ref;
  if (!is_safe_output_ref(artifact.output_ref) || !is_safe_output_ref(plan.waveform.input_ref)) {
    artifact.skipped_reason = "waveform input_ref or output_ref is not a safe relative package path";
    return artifact;
  }

  const std::filesystem::path input_path = staged_path_for_ref(staging_root,
                                                              plan.waveform.input_ref);
  artifact.staged_output_path = staged_path_for_ref(staging_root, artifact.output_ref);
  if (!run.analysis_audio.success || !std::filesystem::exists(input_path)) {
    artifact.skipped_reason = "analysis audio was not staged";
    return artifact;
  }

  try {
    const std::vector<WaveformEnvelopeRecord> records =
        generate_waveform_envelope_records(input_path, plan.waveform.window_duration_us);
    std::vector<nlohmann::json> json_records;
    json_records.reserve(records.size());
    for (const WaveformEnvelopeRecord& record : records) {
      json_records.push_back(waveform_envelope_record_to_json(record));
    }
    write_jsonl_file(artifact.staged_output_path, json_records);
    artifact.written = true;
  } catch (const std::exception& error) {
    artifact.skipped_reason = error.what();
  }

  return artifact;
}

}  // namespace

AudioExtractionRun execute_audio_extraction_plan(const AudioExtractionPlan& plan,
                                                 const std::filesystem::path& staging_root,
                                                 bool suppress_stderr) {
  AudioExtractionRun run;
  run.staging_root = staging_root;
  std::filesystem::create_directories(staging_root);

  for (const AudioExtractionCommandPlan& command : plan.original_streams) {
    AudioExtractionCommandRun command_run =
        execute_command(command.task_id, command.output_ref, command.arguments, staging_root, suppress_stderr);
    run.extraction_run = run.extraction_run || command_run.command_executed;
    if (!command_run.success && !command_run.skipped_reason.empty()) {
      run.blockers.push_back(command.task_id + ": " + command_run.skipped_reason);
    }
    run.original_streams.push_back(std::move(command_run));
  }

  for (const AnalysisAudioCommandPlan& command : plan.microphone_analysis_streams) {
    AudioExtractionCommandRun command_run =
        execute_command(command.task_id, command.output_ref, command.arguments,
                        staging_root, suppress_stderr);
    run.extraction_run = run.extraction_run || command_run.command_executed;
    if (!command_run.success && !command_run.skipped_reason.empty()) {
      run.blockers.push_back(command.task_id + ": " + command_run.skipped_reason);
    }
    run.microphone_analysis_streams.push_back(std::move(command_run));
  }

  run.analysis_audio = execute_command(plan.analysis_audio.task_id,
                                       plan.analysis_audio.output_ref,
                                       plan.analysis_audio.arguments,
                                       staging_root,
                                       suppress_stderr);
  run.extraction_run = run.extraction_run || run.analysis_audio.command_executed;
  if (!run.analysis_audio.success && !run.analysis_audio.skipped_reason.empty()) {
    run.blockers.push_back(plan.analysis_audio.task_id + ": " +
                           run.analysis_audio.skipped_reason);
  }

  run.original_streams_written = !run.original_streams.empty();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    run.original_streams_written = run.original_streams_written && command_run.success;
  }
  run.microphone_analysis_streams_written =
      plan.microphone_analysis_streams.empty() ||
      run.microphone_analysis_streams.size() == plan.microphone_analysis_streams.size();
  for (const AudioExtractionCommandRun& command_run : run.microphone_analysis_streams) {
    run.microphone_analysis_streams_written =
        run.microphone_analysis_streams_written && command_run.success;
  }
  run.analysis_audio_written = run.analysis_audio.success;
  run.audio_absence = stage_audio_absence(plan, run, staging_root);
  run.audio_absence_written = run.audio_absence.written;
  if (!run.audio_absence_written && !run.audio_absence.skipped_reason.empty()) {
    run.blockers.push_back(plan.audio_absence.task_id + ": " +
                           run.audio_absence.skipped_reason);
  }
  run.waveform = stage_waveform_artifact(plan, run, staging_root);
  run.waveform_written = run.waveform.written;
  if (!run.waveform_written && !run.waveform.skipped_reason.empty()) {
    run.blockers.push_back(plan.waveform.task_id + ": " + run.waveform.skipped_reason);
  }
  run.processor_provenance = stage_processor_provenance(plan, run, staging_root);
  run.processor_provenance_written = run.processor_provenance.written;
  if (!run.processor_provenance_written && !run.processor_provenance.skipped_reason.empty()) {
    run.blockers.push_back(plan.processor_provenance.task_id + ": " +
                           run.processor_provenance.skipped_reason);
  }

  return run;
}

nlohmann::json audio_extraction_run_to_json(const AudioExtractionRun& run) {
  nlohmann::json original_streams = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.original_streams) {
    original_streams.push_back(command_run_to_json(command_run));
  }
  nlohmann::json microphone_analysis_streams = nlohmann::json::array();
  for (const AudioExtractionCommandRun& command_run : run.microphone_analysis_streams) {
    microphone_analysis_streams.push_back(command_run_to_json(command_run));
  }

  return {
      {"staging_root", run.staging_root.string()},
      {"original_streams", original_streams},
      {"microphone_analysis_streams", microphone_analysis_streams},
      {"analysis_audio", command_run_to_json(run.analysis_audio)},
      {"audio_absence", derived_artifact_run_to_json(run.audio_absence)},
      {"waveform", derived_artifact_run_to_json(run.waveform)},
      {"processor_provenance",
       derived_artifact_run_to_json(run.processor_provenance)},
      {"blockers", run.blockers},
      {"extraction_run", run.extraction_run},
      {"original_streams_written", run.original_streams_written},
      {"microphone_analysis_streams_written",
       run.microphone_analysis_streams_written},
      {"analysis_audio_written", run.analysis_audio_written},
      {"audio_absence_written", run.audio_absence_written},
      {"waveform_written", run.waveform_written},
      {"processor_provenance_written", run.processor_provenance_written},
  };
}

}  // namespace svp::audio
