// Memory admission (plan §3.5): free memory minus a named reserve must cover
// the task's estimated peak RSS on top of what is already admitted.

#include "svp/exec/worker/admission.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;

constexpr std::uint64_t kGiB = 1ULL << 30;
constexpr std::uint64_t kMiB = 1ULL << 20;

MemorySnapshot available(std::uint64_t bytes, MemoryPressure pressure = MemoryPressure::normal) {
  return MemorySnapshot{.available_bytes = bytes, .pressure = pressure};
}

void test_reserve_scales_with_the_mac() {
  const AdmissionPolicy policy;
  expect(policy.reserve_bytes(8 * kGiB) == kDefaultAdmissionReserveFloorBytes,
         "small Macs keep the floor");
  expect(policy.reserve_bytes(16 * kGiB) == 2 * kGiB, "16 GiB: one eighth equals the floor");
  expect(policy.reserve_bytes(128 * kGiB) == 16 * kGiB, "large Macs keep one eighth");
}

void test_fits_only_above_the_reserve() {
  const AdmissionPolicy policy;
  const std::uint64_t physical = 16 * kGiB;
  expect(decide_admission(policy, physical, available(5 * kGiB), 0, 3 * kGiB).admitted,
         "5 GiB available - 2 GiB reserve covers 3 GiB");
  const AdmissionDecision refused =
      decide_admission(policy, physical, available(5 * kGiB), 0, 3 * kGiB + kMiB);
  expect(!refused.admitted && refused.code == kRejectInsufficientMemory,
         "one MiB more does not fit");
  expect(refused.message.find("reserve") != std::string::npos, "message explains the reserve");
}

void test_admitted_leases_count_until_released() {
  AdmissionLedger ledger(AdmissionPolicy{}, 16 * kGiB);
  const MemorySnapshot memory = available(6 * kGiB);
  expect(ledger.try_admit("s1", "a", 2 * kGiB, memory).admitted, "first lease fits");
  expect(ledger.try_admit("s2", "a", 2 * kGiB, memory).admitted,
         "same lease id in another session is a different lease");
  expect(ledger.committed_bytes() == 4 * kGiB, "both committed");
  expect(!ledger.try_admit("s1", "b", kGiB, memory).admitted,
         "6 - 2 reserve - 4 committed leaves nothing");
  ledger.release("s1", "a");
  expect(ledger.try_admit("s1", "b", kGiB, memory).admitted, "released memory is reusable");
  ledger.release_session("s1");
  ledger.release_session("s2");
  expect(ledger.committed_bytes() == 0, "sessions release everything");
}

void test_pressure_refuses_regardless_of_memory() {
  const AdmissionPolicy policy;
  const AdmissionDecision warning = decide_admission(
      policy, 16 * kGiB, available(12 * kGiB, MemoryPressure::warning), 0, kMiB);
  expect(!warning.admitted && warning.code == kRejectMemoryPressure, "warning refuses");
  AdmissionPolicy lenient;
  lenient.refuse_at = MemoryPressure::critical;
  expect(decide_admission(lenient, 16 * kGiB, available(12 * kGiB, MemoryPressure::warning), 0,
                          kMiB)
             .admitted,
         "the threshold is a policy field");
}

void test_unknown_estimate_needs_room_above_the_reserve() {
  const AdmissionPolicy policy;
  expect(decide_admission(policy, 16 * kGiB, available(3 * kGiB), 0, 0).admitted,
         "no estimate: admitted while memory exceeds the reserve");
  expect(!decide_admission(policy, 16 * kGiB, available(2 * kGiB), 0, 0).admitted,
         "no estimate: refused at the reserve");
}

void test_sampled_memory_is_plausible() {
  const HostFacts host = detect_host_facts();
  const MemorySnapshot memory = sample_memory();
  expect(memory.available_bytes > 0 && memory.available_bytes <= host.physical_memory_bytes,
         "available memory is within physical memory");
  expect(host.logical_cpus > 0 && !host.os.product_version.empty(), "host facts are read");
}

}  // namespace

int main() {
  return run_tests("svp-exec-worker-admission-tests",
                   {
                       {"reserve scales with the Mac", test_reserve_scales_with_the_mac},
                       {"fits only above the reserve", test_fits_only_above_the_reserve},
                       {"admitted leases count until released",
                        test_admitted_leases_count_until_released},
                       {"pressure refuses regardless of memory",
                        test_pressure_refuses_regardless_of_memory},
                       {"unknown estimate needs room above the reserve",
                        test_unknown_estimate_needs_room_above_the_reserve},
                       {"sampled memory is plausible", test_sampled_memory_is_plausible},
                   });
}
