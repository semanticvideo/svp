#pragma once

// What a coordinator and a worker tell each other about the Mac they run on
// (plan §3.5, §4.1 `os`, §4.3 HELLO). Every value is read from the system at
// run time; nothing here names, counts, or assumes a particular machine.
// The product version decides compatibility (a worker must run the
// coordinator's macOS version, plan §0 R1); CPU and memory figures feed
// admission now and capacity calibration later; `cpu_brand` is for reports
// only and never selects behaviour.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace svp::exec::worker {

struct OsIdentity {
  // `sw_vers -productVersion`, e.g. "27.0.1": must match across a pairing.
  std::string product_version;
  // `sw_vers -buildVersion`, e.g. "26A434": reported, not compared (a
  // supplemental update can change it without changing behaviour SVP uses).
  std::string build;

  bool operator==(const OsIdentity&) const = default;
};

struct HostFacts {
  // "arm64" on Apple Silicon (plan §0: Intel Macs are out of scope).
  std::string arch;
  OsIdentity os;
  // hw.logicalcpu, and the performance / efficiency split
  // (hw.perflevel0/1.logicalcpu); a split value is 0 when the system does
  // not report it.
  std::uint32_t logical_cpus = 0;
  std::uint32_t performance_cpus = 0;
  std::uint32_t efficiency_cpus = 0;
  // hw.memsize.
  std::uint64_t physical_memory_bytes = 0;
  // machdep.cpu.brand_string, for reports only.
  std::string cpu_brand;

  bool operator==(const HostFacts&) const = default;
};

// The kernel's own memory pressure level (kern.memorystatus_vm_pressure_level).
enum class MemoryPressure { normal, warning, critical };

[[nodiscard]] std::string_view memory_pressure_name(MemoryPressure pressure) noexcept;
[[nodiscard]] std::optional<MemoryPressure> parse_memory_pressure(std::string_view name) noexcept;

struct MemorySnapshot {
  // Memory the system can hand to a new process without paging anything
  // out: free, speculative, inactive (clean file cache the kernel reclaims
  // first), and purgeable pages. Compressed and wired pages are excluded.
  std::uint64_t available_bytes = 0;
  MemoryPressure pressure = MemoryPressure::normal;

  bool operator==(const MemorySnapshot&) const = default;
};

// Throws WorkerError(io) when a required value cannot be read.
[[nodiscard]] HostFacts detect_host_facts();
[[nodiscard]] MemorySnapshot sample_memory();

// Bytes available to this user on the volume holding `path` (or its nearest
// existing parent). Throws WorkerError(io).
[[nodiscard]] std::uint64_t available_disk_bytes(const std::filesystem::path& path);

}  // namespace svp::exec::worker
