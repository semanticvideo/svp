#include "svp/exec/worker/host_facts.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <mach/mach.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#include <system_error>
#include <vector>

namespace svp::exec::worker {
namespace {

std::optional<std::string> sysctl_string(const char* name) {
  std::size_t size = 0;
  if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return std::nullopt;
  }
  std::vector<char> buffer(size);
  if (::sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0) {
    return std::nullopt;
  }
  return std::string(buffer.data());
}

template <typename T>
std::optional<T> sysctl_value(const char* name) {
  T value{};
  std::size_t size = sizeof(value);
  if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value)) {
    return std::nullopt;
  }
  return value;
}

std::string required_string(const char* name) {
  std::optional<std::string> value = sysctl_string(name);
  if (!value || value->empty()) {
    throw WorkerError(WorkerErrorCode::io, std::string("cannot read sysctl ") + name);
  }
  return *value;
}

}  // namespace

std::string_view memory_pressure_name(MemoryPressure pressure) noexcept {
  switch (pressure) {
    case MemoryPressure::normal:
      return "normal";
    case MemoryPressure::warning:
      return "warning";
    case MemoryPressure::critical:
      return "critical";
  }
  return "unknown";
}

std::optional<MemoryPressure> parse_memory_pressure(std::string_view name) noexcept {
  for (const MemoryPressure pressure :
       {MemoryPressure::normal, MemoryPressure::warning, MemoryPressure::critical}) {
    if (memory_pressure_name(pressure) == name) {
      return pressure;
    }
  }
  return std::nullopt;
}

HostFacts detect_host_facts() {
  HostFacts facts;
  utsname name{};
  if (::uname(&name) != 0) {
    throw WorkerError(WorkerErrorCode::io, "uname failed");
  }
  facts.arch = name.machine;
  facts.os.product_version = required_string("kern.osproductversion");
  facts.os.build = required_string("kern.osversion");
  const auto logical = sysctl_value<std::int32_t>("hw.logicalcpu");
  const auto memory = sysctl_value<std::uint64_t>("hw.memsize");
  if (!logical || *logical <= 0 || !memory || *memory == 0) {
    throw WorkerError(WorkerErrorCode::io, "cannot read hw.logicalcpu / hw.memsize");
  }
  facts.logical_cpus = static_cast<std::uint32_t>(*logical);
  facts.physical_memory_bytes = *memory;
  if (const auto performance = sysctl_value<std::int32_t>("hw.perflevel0.logicalcpu");
      performance && *performance > 0) {
    facts.performance_cpus = static_cast<std::uint32_t>(*performance);
  }
  if (const auto efficiency = sysctl_value<std::int32_t>("hw.perflevel1.logicalcpu");
      efficiency && *efficiency > 0) {
    facts.efficiency_cpus = static_cast<std::uint32_t>(*efficiency);
  }
  facts.cpu_brand = sysctl_string("machdep.cpu.brand_string").value_or("");
  return facts;
}

MemorySnapshot sample_memory() {
  vm_statistics64_data_t statistics{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  const mach_port_t host = ::mach_host_self();
  const kern_return_t status = ::host_statistics64(
      host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count);
  vm_size_t page_size = 0;
  ::host_page_size(host, &page_size);
  ::mach_port_deallocate(::mach_task_self(), host);
  if (status != KERN_SUCCESS || page_size == 0) {
    throw WorkerError(WorkerErrorCode::io, "host_statistics64 failed");
  }
  const std::uint64_t pages = static_cast<std::uint64_t>(statistics.free_count) +
                              statistics.speculative_count + statistics.inactive_count +
                              statistics.purgeable_count;
  MemorySnapshot snapshot;
  snapshot.available_bytes = pages * static_cast<std::uint64_t>(page_size);
  // kern.memorystatus_vm_pressure_level: 1 normal, 2 warning, 4 critical
  // (the kernel's DISPATCH_MEMORYPRESSURE_* values).
  const auto level = sysctl_value<std::int32_t>("kern.memorystatus_vm_pressure_level");
  if (level && *level >= 4) {
    snapshot.pressure = MemoryPressure::critical;
  } else if (level && *level >= 2) {
    snapshot.pressure = MemoryPressure::warning;
  }
  return snapshot;
}

std::uint64_t available_disk_bytes(const std::filesystem::path& path) {
  std::filesystem::path probe = path;
  std::error_code error;
  while (!probe.empty() && !std::filesystem::exists(probe, error)) {
    if (probe == probe.parent_path()) {
      break;
    }
    probe = probe.parent_path();
  }
  struct statvfs stats{};
  if (probe.empty() || ::statvfs(probe.c_str(), &stats) != 0) {
    throw WorkerError(WorkerErrorCode::io, "cannot stat the volume holding " + path.string());
  }
  return static_cast<std::uint64_t>(stats.f_bavail) * static_cast<std::uint64_t>(stats.f_frsize);
}

}  // namespace svp::exec::worker
