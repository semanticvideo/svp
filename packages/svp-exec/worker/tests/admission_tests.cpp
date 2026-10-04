// Memory admission (plan §3.5): free memory minus a named reserve must cover
// the task's estimated peak RSS on top of what running leases are still
// expected to take beyond the memory their session processes already use.
// Running leases are counted once; a session whose process memory cannot be
// read counts its estimates in full.

#include "svp/exec/worker/admission.hpp"
#include "svp/exec/worker/session_memory.hpp"
#include "worker_test_support.hpp"

#include <map>
#include <unistd.h>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;

constexpr std::uint64_t kGiB = 1ULL << 30;
constexpr std::uint64_t kMiB = 1ULL << 20;

constexpr std::string_view kOcr = "ocr.frame_batch";
constexpr std::string_view kAsr = "asr.chunk_batch";

MemorySnapshot available(std::uint64_t bytes, MemoryPressure pressure = MemoryPressure::normal) {
  return MemorySnapshot{.available_bytes = bytes, .pressure = pressure};
}

AdmissionDecision admit(AdmissionLedger& ledger, std::string_view session,
                        std::string_view lease_id, std::uint64_t bytes,
                        const MemorySnapshot& memory, const SessionInUse& in_use = {},
                        std::string_view task_type = kOcr) {
  return ledger.try_admit(LeaseAdmission{.session = session,
                                         .lease_id = lease_id,
                                         .task_type = task_type,
                                         .required_bytes = bytes},
                          memory, in_use);
}

// Memory in use per session; sessions not listed are unreadable.
SessionInUse in_use(std::map<std::string, std::uint64_t, std::less<>> sessions) {
  return [sessions = std::move(sessions)](std::string_view session) {
    const auto found = sessions.find(session);
    return found == sessions.end() ? std::nullopt
                                   : std::optional<std::uint64_t>(found->second);
  };
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
  // No session memory readable: estimates count in full.
  AdmissionLedger ledger(AdmissionPolicy{}, 16 * kGiB);
  const MemorySnapshot memory = available(6 * kGiB);
  expect(admit(ledger, "s1", "a", 2 * kGiB, memory).admitted, "first lease fits");
  expect(admit(ledger, "s2", "a", 2 * kGiB, memory).admitted,
         "same lease id in another session is a different lease");
  expect(ledger.committed_bytes() == 4 * kGiB, "both committed");
  expect(!admit(ledger, "s1", "b", kGiB, memory).admitted,
         "6 - 2 reserve - 4 committed leaves nothing");
  ledger.release("s1", "a");
  expect(admit(ledger, "s1", "b", kGiB, memory).admitted, "released memory is reusable");
  ledger.release_session("s1");
  ledger.release_session("s2");
  expect(ledger.committed_bytes() == 0, "sessions release everything");
}

void test_pending_bytes_never_go_below_zero() {
  expect(pending_bytes(3 * kGiB, std::nullopt) == 3 * kGiB, "unknown use: the full estimate");
  expect(pending_bytes(3 * kGiB, kGiB) == 2 * kGiB, "what the process has not reached yet");
  expect(pending_bytes(3 * kGiB, 3 * kGiB) == 0, "reached");
  expect(pending_bytes(kGiB, 5 * kGiB) == 0, "a process above its estimates owes nothing");
  expect(pending_bytes(0, kGiB) == 0, "no running leases");
}

void test_running_leases_count_once() {
  // A 16 GiB worker (2 GiB reserve) runs three 1500 MiB OCR leases in one
  // session whose process already uses 4000 MiB; 6370 MiB are available
  // with that memory already gone. An ASR lease of 2304 MiB fits: only the
  // 500 MiB the OCR leases have not reached yet is still spoken for.
  AdmissionLedger ledger(AdmissionPolicy{}, 16 * kGiB);
  const MemorySnapshot memory = available(6370 * kMiB);
  for (const char* lease : {"o1", "o2", "o3"}) {
    expect(admit(ledger, "s1", lease, 1500 * kMiB, available(64 * kGiB)).admitted,
           "OCR lease admitted while memory was plentiful");
  }
  const SessionInUse readable = in_use({{"s1", 4000 * kMiB}});
  expect(admit(ledger, "s1", "a1", 2304 * kMiB, memory, readable, kAsr).admitted,
         "6370 - 2048 reserve - 500 pending covers 2304");
  ledger.release("s1", "a1");

  // Per session: a process above its own estimates is no credit for another
  // session's leases. s2 runs 2 x 1500 MiB and uses 1000 MiB (2000 pending);
  // s1's surplus does not cancel that.
  for (const char* lease : {"p1", "p2"}) {
    expect(admit(ledger, "s2", lease, 1500 * kMiB, available(64 * kGiB)).admitted,
           "second session's lease admitted while memory was plentiful");
  }
  const SessionInUse both = in_use({{"s1", 9000 * kMiB}, {"s2", 1000 * kMiB}});
  // 6370 - 2048 - (0 + 2000) = 2322 MiB free.
  expect(admit(ledger, "s3", "b1", 2322 * kMiB, memory, both).admitted,
         "fits exactly with s2's pending counted and s1's surplus ignored");
  ledger.release("s3", "b1");
  expect(!admit(ledger, "s3", "b2", 2322 * kMiB + kMiB, memory, both).admitted,
         "one MiB more does not fit");

  // Counting the running estimates in full (the old rule, and the fallback
  // when no process memory can be read) refuses the ASR lease above.
  AdmissionLedger full(AdmissionPolicy{}, 16 * kGiB);
  for (const char* lease : {"o1", "o2", "o3"}) {
    expect(admit(full, "s1", lease, 1500 * kMiB, available(64 * kGiB)).admitted, "OCR lease");
  }
  const AdmissionDecision refused = admit(full, "s1", "a1", 2304 * kMiB, memory, {}, kAsr);
  expect(!refused.admitted && refused.code == kRejectInsufficientMemory,
         "estimates in full: 6370 - 2048 - 4500 cannot cover 2304");
}

void test_unreadable_session_counts_in_full() {
  // Two sessions with 2 GiB of estimates each; only s1's process is readable
  // (and has reached its estimates). s2's estimates count in full.
  AdmissionLedger ledger(AdmissionPolicy{}, 16 * kGiB);
  expect(admit(ledger, "s1", "a", 2 * kGiB, available(64 * kGiB)).admitted, "s1 lease");
  expect(admit(ledger, "s2", "a", 2 * kGiB, available(64 * kGiB)).admitted, "s2 lease");
  const MemorySnapshot memory = available(6 * kGiB);
  const SessionInUse partial = in_use({{"s1", 2 * kGiB}});
  // 6 - 2 reserve - (0 + 2) = 2 GiB free.
  expect(admit(ledger, "s3", "a", 2 * kGiB, memory, partial).admitted,
         "s1 counted once, s2 in full");
  ledger.release("s3", "a");
  expect(!admit(ledger, "s3", "b", 2 * kGiB + kMiB, memory, partial).admitted,
         "s2's unread estimate is still spoken for");
  expect(!admit(ledger, "s3", "c", kGiB, memory).admitted,
         "nothing readable: 6 - 2 - 4 leaves nothing, as before");
}

void test_session_memory_reads_attached_processes() {
  expect(process_resident_bytes(::getpid()).value_or(0) > 0, "this process has resident memory");
  expect(!process_resident_bytes(-1).has_value(), "no process: unknown");
  SessionMemory sessions([](pid_t pid) { return std::optional<std::uint64_t>(pid * kMiB); });
  expect(!sessions.in_use("s1").has_value(), "not attached: unknown");
  sessions.attach("s1", 7);
  expect(sessions.in_use("s1") == 7 * kMiB, "attached: the probe's figure");
  sessions.detach("s1");
  sessions.detach("s1");
  expect(!sessions.in_use("s1").has_value(), "detached: unknown");
  SessionMemory unreadable([](pid_t) { return std::optional<std::uint64_t>(); });
  unreadable.attach("s1", 7);
  expect(!unreadable.in_use("s1").has_value(), "a probe that cannot read gives unknown");
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
                       {"pending bytes never go below zero",
                        test_pending_bytes_never_go_below_zero},
                       {"running leases count once", test_running_leases_count_once},
                       {"unreadable session counts in full",
                        test_unreadable_session_counts_in_full},
                       {"session memory reads attached processes",
                        test_session_memory_reads_attached_processes},
                       {"pressure refuses regardless of memory",
                        test_pressure_refuses_regardless_of_memory},
                       {"unknown estimate needs room above the reserve",
                        test_unknown_estimate_needs_room_above_the_reserve},
                       {"sampled memory is plausible", test_sampled_memory_is_plausible},
                   });
}
