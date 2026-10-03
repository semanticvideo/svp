#include "svp/exec/worker/launchd_job.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <sstream>

namespace svp::exec::worker {
namespace {

std::string xml_escape(std::string_view text) {
  std::string escaped;
  for (const char c : text) {
    switch (c) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

void key_string(std::ostringstream& out, std::string_view indent, std::string_view key,
                std::string_view value) {
  out << indent << "<key>" << xml_escape(key) << "</key>\n"
      << indent << "<string>" << xml_escape(value) << "</string>\n";
}

}  // namespace

WorkerServiceSpec make_worker_service_spec(WorkerServiceMode mode, const WorkerLayout& layout,
                                           const std::string& user_name,
                                           const std::filesystem::path& home, std::string path) {
  WorkerServiceSpec spec;
  spec.mode = mode;
  spec.program_arguments = {layout.service_program().string(), "worker", "serve", "--root",
                            layout.root.string()};
  spec.working_directory = layout.root;
  spec.log_path = layout.agent_log();
  spec.user_name = user_name;
  spec.home = home;
  spec.path = std::move(path);
  return spec;
}

std::string render_launchd_plist(const WorkerServiceSpec& spec) {
  if (spec.label.empty() || spec.program_arguments.empty() ||
      spec.program_arguments.front().empty() ||
      !std::filesystem::path(spec.program_arguments.front()).is_absolute()) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "a launchd job needs a label and an absolute program path");
  }
  if (spec.mode == WorkerServiceMode::system_daemon &&
      (spec.user_name.empty() || spec.home.empty())) {
    throw WorkerError(WorkerErrorCode::configuration,
                      "a LaunchDaemon worker needs the user it runs as and that user's home");
  }
  std::ostringstream out;
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n<dict>\n";
  constexpr std::string_view kIndent = "  ";
  if (spec.mode == WorkerServiceMode::system_daemon || !spec.path.empty()) {
    out << kIndent << "<key>EnvironmentVariables</key>\n"
        << kIndent << "<dict>\n";
    if (spec.mode == WorkerServiceMode::system_daemon) {
      key_string(out, "    ", "HOME", spec.home.string());
    }
    if (!spec.path.empty()) {
      key_string(out, "    ", "PATH", spec.path);
    }
    out << kIndent << "</dict>\n";
  }
  out << kIndent << "<key>KeepAlive</key>\n"
      << kIndent << "<dict>\n"
      << "    <key>SuccessfulExit</key>\n"
      << "    <false/>\n"
      << kIndent << "</dict>\n";
  key_string(out, kIndent, "Label", spec.label);
  key_string(out, kIndent, "ProcessType", "Standard");
  out << kIndent << "<key>ProgramArguments</key>\n" << kIndent << "<array>\n";
  for (const std::string& argument : spec.program_arguments) {
    out << "    <string>" << xml_escape(argument) << "</string>\n";
  }
  out << kIndent << "</array>\n"
      << kIndent << "<key>RunAtLoad</key>\n"
      << kIndent << "<true/>\n";
  if (!spec.log_path.empty()) {
    key_string(out, kIndent, "StandardErrorPath", spec.log_path.string());
    key_string(out, kIndent, "StandardOutPath", spec.log_path.string());
  }
  out << kIndent << "<key>ThrottleInterval</key>\n"
      << kIndent << "<integer>" << kWorkerJobThrottleSeconds << "</integer>\n"
      << kIndent << "<key>Umask</key>\n"
      << kIndent << "<integer>" << kWorkerJobUmask << "</integer>\n";
  if (spec.mode == WorkerServiceMode::system_daemon) {
    key_string(out, kIndent, "UserName", spec.user_name);
  }
  if (!spec.working_directory.empty()) {
    key_string(out, kIndent, "WorkingDirectory", spec.working_directory.string());
  }
  out << "</dict>\n</plist>\n";
  return out.str();
}

}  // namespace svp::exec::worker
