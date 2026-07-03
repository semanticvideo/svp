#include "svp/core/memory_diagnostics.hpp"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace svp::core {
namespace {

std::mutex g_mutex;
std::filesystem::path g_log_path;
std::uint64_t g_limit_bytes = 0;
bool g_configured = false;
bool g_enabled = false;
std::uint64_t g_peak_resident_bytes = 0;
std::uint64_t g_peak_footprint_bytes = 0;
const std::chrono::steady_clock::time_point g_start_time =
    std::chrono::steady_clock::now();

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream oss;
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(ch));
          out += oss.str();
        } else {
          out += ch;
        }
        break;
    }
  }
  return out;
}

std::string now_utc_iso8601() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto time = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::uint64_t parse_mb_env(const char* value) {
  if (value == nullptr || *value == '\0') return 0;
  char* end = nullptr;
  const unsigned long long mb = std::strtoull(value, &end, 10);
  if (end == value || mb == 0) return 0;
  return static_cast<std::uint64_t>(mb) * 1024ull * 1024ull;
}

std::string bytes_to_string(std::uint64_t value) {
  return std::to_string(value);
}

std::uint64_t elapsed_ms_since_start() {
  const auto elapsed = std::chrono::steady_clock::now() - g_start_time;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void write_event_locked(
    std::string_view scope,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const MemorySnapshot& snapshot,
    bool limit_exceeded) {
  if (!g_enabled || g_log_path.empty()) return;
  std::filesystem::create_directories(g_log_path.parent_path());
  std::ofstream out(g_log_path, std::ios::app);
  if (!out) return;

  g_peak_resident_bytes = std::max(g_peak_resident_bytes, snapshot.resident_bytes);
  g_peak_footprint_bytes =
      std::max(g_peak_footprint_bytes, snapshot.physical_footprint_bytes);

  out << "{";
  out << "\"ts\":\"" << now_utc_iso8601() << "\"";
#if defined(__APPLE__)
  out << ",\"pid\":" << static_cast<long long>(getpid());
#endif
  out << ",\"thread\":\"" << json_escape(
      std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))) << "\"";
  out << ",\"scope\":\"" << json_escape(scope) << "\"";
  out << ",\"elapsed_ms\":" << elapsed_ms_since_start();
  out << ",\"rss_bytes\":" << snapshot.resident_bytes;
  out << ",\"vm_bytes\":" << snapshot.virtual_bytes;
  out << ",\"footprint_bytes\":" << snapshot.physical_footprint_bytes;
  out << ",\"peak_rss_bytes\":" << g_peak_resident_bytes;
  out << ",\"peak_footprint_bytes\":" << g_peak_footprint_bytes;
  out << ",\"user_cpu_ms\":" << snapshot.user_cpu_ms;
  out << ",\"system_cpu_ms\":" << snapshot.system_cpu_ms;
  out << ",\"total_cpu_ms\":" << snapshot.total_cpu_ms;
  out << ",\"limit_bytes\":" << g_limit_bytes;
  out << ",\"limit_exceeded\":" << (limit_exceeded ? "true" : "false");
  for (const auto& [key, value] : fields) {
    out << ",\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
  }
  out << "}\n";
  out.flush();
}

std::vector<std::pair<std::string, std::string>> copy_fields(
    std::initializer_list<std::pair<std::string, std::string>> fields) {
  std::vector<std::pair<std::string, std::string>> copied;
  copied.reserve(fields.size());
  for (const auto& [key, value] : fields) {
    copied.emplace_back(std::string(key), std::string(value));
  }
  return copied;
}

}  // namespace

MemorySnapshot current_memory_snapshot() {
  MemorySnapshot snapshot;
#if defined(__APPLE__)
  mach_task_basic_info_data_t basic{};
  mach_msg_type_number_t basic_count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(),
                MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&basic),
                &basic_count) == KERN_SUCCESS) {
    snapshot.resident_bytes = static_cast<std::uint64_t>(basic.resident_size);
    snapshot.virtual_bytes = static_cast<std::uint64_t>(basic.virtual_size);
  }

  task_vm_info_data_t vm{};
  mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(),
                TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&vm),
                &vm_count) == KERN_SUCCESS) {
    snapshot.physical_footprint_bytes =
      static_cast<std::uint64_t>(vm.phys_footprint);
  }

  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    const auto user_ms =
        static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000ull +
        static_cast<std::uint64_t>(usage.ru_utime.tv_usec) / 1000ull;
    const auto system_ms =
        static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000ull +
        static_cast<std::uint64_t>(usage.ru_stime.tv_usec) / 1000ull;
    snapshot.user_cpu_ms = user_ms;
    snapshot.system_cpu_ms = system_ms;
    snapshot.total_cpu_ms = user_ms + system_ms;
  }
#endif
  return snapshot;
}

void configure_memory_diagnostics_from_environment(
    const std::filesystem::path& default_log_path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_configured) return;
  g_configured = true;

  const char* env_log = std::getenv("SVP_BUILDER_DIAG_LOG");
  if (env_log != nullptr && *env_log != '\0') {
    g_log_path = env_log;
  } else {
    g_log_path = default_log_path;
  }

  g_limit_bytes = parse_mb_env(std::getenv("SVP_BUILDER_MEMORY_LIMIT_MB"));
  const char* enabled = std::getenv("SVP_BUILDER_DIAG");
  g_enabled = !g_log_path.empty() &&
      ((enabled != nullptr && std::string_view(enabled) != "0") ||
       env_log != nullptr ||
       g_limit_bytes > 0);

  if (g_limit_bytes == 0) {
    g_limit_bytes = 36ull * 1024ull * 1024ull * 1024ull;
  }

  if (g_enabled) {
    const MemorySnapshot snapshot = current_memory_snapshot();
    write_event_locked("diagnostics.configure", {}, snapshot, false);
  }
}

void configure_memory_diagnostics(
    const std::filesystem::path& log_path,
    std::uint64_t limit_bytes) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_configured = true;
  g_log_path = log_path;
  g_limit_bytes = limit_bytes;
  g_enabled = !g_log_path.empty();
  const MemorySnapshot snapshot = current_memory_snapshot();
  write_event_locked("diagnostics.configure", {}, snapshot, false);
}

bool memory_diagnostics_enabled() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_enabled;
}

std::filesystem::path memory_diagnostics_log_path() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_log_path;
}

std::uint64_t memory_diagnostics_limit_bytes() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_limit_bytes;
}

void trace_memory_event(
    std::string_view scope,
    std::initializer_list<std::pair<std::string, std::string>> fields) {
  trace_memory_event(scope, copy_fields(fields));
}

void trace_memory_event(
    std::string_view scope,
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled) return;
  write_event_locked(scope, fields, current_memory_snapshot(), false);
}

void check_memory_limit(
    std::string_view scope,
    std::initializer_list<std::pair<std::string, std::string>> fields) {
  check_memory_limit(scope, copy_fields(fields));
}

void check_memory_limit(
    std::string_view scope,
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled) return;
  const MemorySnapshot snapshot = current_memory_snapshot();
  const std::uint64_t comparable =
      snapshot.physical_footprint_bytes > 0
          ? snapshot.physical_footprint_bytes
          : snapshot.resident_bytes;
  const bool exceeded = g_limit_bytes > 0 && comparable >= g_limit_bytes;
  write_event_locked(scope, fields, snapshot, exceeded);
  if (exceeded) {
    std::_Exit(86);
  }
}

}  // namespace svp::core
