#include "svp/exec/worker/local_load.hpp"

#include "svp/exec/worker/pairing_store.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <fstream>
#include <optional>
#include <sstream>
#include <unistd.h>

namespace svp::exec::worker {
namespace {

constexpr std::string_view kLocalLoadDirName = "LocalLoad";
constexpr std::string_view kLocalLoadExtension = ".json";

bool process_alive(pid_t pid) {
  return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

struct LoadRecord {
  pid_t pid = 0;
  TaskTypeCounts running;
};

std::optional<LoadRecord> read_record(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  const nlohmann::json body = nlohmann::json::parse(text.str(), nullptr, false);
  if (!body.is_object() || body.value("schema", std::string()) != kLocalLoadSchema) {
    return std::nullopt;
  }
  const auto pid = body.find("pid");
  const auto running = body.find("running");
  if (pid == body.end() || !pid->is_number_unsigned() || running == body.end() ||
      !running->is_object()) {
    return std::nullopt;
  }
  LoadRecord record{.pid = static_cast<pid_t>(pid->get<std::uint64_t>()), .running = {}};
  for (const auto& [task_type, count] : running->items()) {
    if (!count.is_number_unsigned()) {
      return std::nullopt;
    }
    record.running[task_type] = count.get<std::uint64_t>();
  }
  return record;
}

}  // namespace

std::filesystem::path default_local_load_dir() {
  return default_coordinator_pairings_dir().parent_path() / kLocalLoadDirName;
}

TaskTypeCounts read_local_load(const std::filesystem::path& directory) {
  TaskTypeCounts total;
  std::error_code error;
  for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
       entry.increment(error)) {
    if (entry->path().extension() != kLocalLoadExtension) {
      continue;
    }
    const std::optional<LoadRecord> record = read_record(entry->path());
    if (!record || !process_alive(record->pid)) {
      continue;
    }
    for (const auto& [task_type, count] : record->running) {
      total[task_type] += count;
    }
  }
  return total;
}

LocalLoadRecorder::LocalLoadRecorder(std::filesystem::path directory)
    : directory_(std::move(directory)),
      file_(directory_ / (std::to_string(::getpid()) + std::string(kLocalLoadExtension))) {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  // Files left by builds that died without removing theirs.
  for (std::filesystem::directory_iterator entry(directory_, error), end; !error && entry != end;
       entry.increment(error)) {
    if (entry->path().extension() != kLocalLoadExtension || entry->path() == file_) {
      continue;
    }
    const std::optional<LoadRecord> record = read_record(entry->path());
    if (record && !process_alive(record->pid)) {
      std::error_code ignored;
      std::filesystem::remove(entry->path(), ignored);
    }
  }
  const std::lock_guard lock(mutex_);
  write_locked();
}

LocalLoadRecorder::~LocalLoadRecorder() {
  std::error_code error;
  std::filesystem::remove(file_, error);
}

void LocalLoadRecorder::add(std::string_view task_type) {
  const std::lock_guard lock(mutex_);
  ++running_[std::string(task_type)];
  write_locked();
}

void LocalLoadRecorder::remove(std::string_view task_type) {
  const std::lock_guard lock(mutex_);
  const auto found = running_.find(task_type);
  if (found == running_.end()) {
    return;
  }
  if (--found->second == 0) {
    running_.erase(found);
  }
  write_locked();
}

void LocalLoadRecorder::write_locked() {
  nlohmann::json running = nlohmann::json::object();
  for (const auto& [task_type, count] : running_) {
    running[task_type] = count;
  }
  const nlohmann::json body{{"pid", static_cast<std::uint64_t>(::getpid())},
                            {"running", std::move(running)},
                            {"schema", std::string(kLocalLoadSchema)}};
  const std::filesystem::path temporary = file_.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      return;
    }
    out << body.dump();
    if (!out) {
      return;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, file_, error);
}

}  // namespace svp::exec::worker
