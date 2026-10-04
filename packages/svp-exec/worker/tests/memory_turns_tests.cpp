// Memory turns (memory_turns.hpp) through the admission ledger: a type
// refused for memory gets the memory the other types free instead of a
// stream of smaller leases taking it again; the others are held back only
// while they hold leases here, only until the waiting type is admitted or
// stops asking, and for a bounded time when its wait cannot help.

#include "svp/exec/worker/admission.hpp"
#include "worker_test_support.hpp"

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
using namespace std::chrono_literals;

constexpr std::uint64_t kGiB = 1ULL << 30;
constexpr std::uint64_t kMiB = 1ULL << 20;
// A 16 GiB Mac keeps a 2 GiB reserve (admission.hpp).
constexpr std::uint64_t kPhysical = 16 * kGiB;

constexpr std::string_view kOcr = "ocr.frame_batch";
constexpr std::string_view kAsr = "asr.chunk_batch";
constexpr std::string_view kTrack = "track.window";

constexpr std::uint64_t kOcrBytes = kGiB;
constexpr std::uint64_t kAsrBytes = 2 * kGiB;

struct FakeClock {
  std::chrono::steady_clock::time_point now{};
  AdmissionLedger::Clock function() {
    return [this] { return now; };
  }
};

MemorySnapshot available(std::uint64_t bytes) {
  return MemorySnapshot{.available_bytes = bytes, .pressure = MemoryPressure::normal};
}

// Every lease in one session; no process memory readable, so the ledger
// counts estimates in full and the arithmetic below is exact.
AdmissionDecision ask(AdmissionLedger& ledger, std::string_view type, std::string_view lease,
                      std::uint64_t bytes, std::uint64_t available_bytes) {
  return ledger.try_admit(LeaseAdmission{.session = "s",
                                         .lease_id = lease,
                                         .task_type = type,
                                         .required_bytes = bytes},
                          available(available_bytes));
}

// 6 GiB available - 2 GiB reserve: 4 GiB for leases.
constexpr std::uint64_t kAvailable = 6 * kGiB;

// Three OCR leases leave 1 GiB: an ASR lease (2 GiB) is refused and waits.
void start_ocr_and_refuse_asr(AdmissionLedger& ledger) {
  for (const char* lease : {"o1", "o2", "o3"}) {
    expect(ask(ledger, kOcr, lease, kOcrBytes, kAvailable).admitted, "OCR lease fits");
  }
  const AdmissionDecision asr = ask(ledger, kAsr, "a1", kAsrBytes, kAvailable);
  expect(!asr.admitted && asr.code == kRejectInsufficientMemory, "ASR does not fit");
  expect(ledger.waiting(kAsr), "ASR waits for its turn");
}

void test_ocr_stream_yields_to_waiting_asr() {
  FakeClock clock;
  AdmissionLedger ledger(AdmissionPolicy{}, kPhysical, {}, clock.function());
  start_ocr_and_refuse_asr(ledger);

  ledger.release("s", "o1");
  clock.now += 100ms;
  // 2 GiB free: another OCR lease would fit, but the freed memory is ASR's.
  const AdmissionDecision ocr = ask(ledger, kOcr, "o4", kOcrBytes, kAvailable);
  expect(!ocr.admitted && ocr.code == kRejectInsufficientMemory,
         "OCR yields while ASR waits");
  expect(ocr.message.find(std::string(kAsr)) != std::string::npos,
         "the refusal names the waiting type");

  clock.now += 2s;  // ASR's rejection backoff.
  expect(ask(ledger, kAsr, "a2", kAsrBytes, kAvailable).admitted, "ASR gets the freed memory");
  expect(!ledger.waiting(kAsr), "admitted: the wait is over");

  // Nothing is held back any more; OCR runs as memory frees.
  ledger.release("s", "o2");
  expect(ask(ledger, kOcr, "o5", kOcrBytes, kAvailable).admitted, "OCR runs again");
}

void test_only_types_holding_leases_yield() {
  FakeClock clock;
  AdmissionLedger ledger(AdmissionPolicy{}, kPhysical, {}, clock.function());
  start_ocr_and_refuse_asr(ledger);
  expect(ask(ledger, kTrack, "t1", 512 * kMiB, kAvailable).admitted,
         "a type with no lease here is not held back");
  const AdmissionDecision second = ask(ledger, kTrack, "t2", 256 * kMiB, kAvailable);
  expect(!second.admitted && second.code == kRejectInsufficientMemory,
         "once it holds a lease it yields too");
  expect(ask(ledger, kAsr, "a2", 0, kAvailable).admitted,
         "the waiting type itself is never held back by its own wait");
}

void test_wait_ends_when_the_waiting_type_stops_asking() {
  FakeClock clock;
  const MemoryTurnPolicy turns;
  AdmissionLedger ledger(AdmissionPolicy{}, kPhysical, turns, clock.function());
  start_ocr_and_refuse_asr(ledger);
  ledger.release("s", "o1");
  clock.now += turns.idle_window;
  expect(!ask(ledger, kOcr, "o4", kOcrBytes, kAvailable).admitted,
         "within the idle window OCR still yields");
  clock.now += 1ms;
  expect(ask(ledger, kOcr, "o5", kOcrBytes, kAvailable).admitted,
         "ASR stopped asking: OCR takes the memory");
  expect(!ledger.waiting(kAsr), "the wait expired");
}

void test_fruitless_wait_ends_and_rests() {
  // ASR waits for o1..o3. They all end, but the Mac's other work took the
  // memory meanwhile: ASR still does not fit, so holding OCR back cannot
  // help. The wait ends, and ASR rests as long as it held OCR back.
  FakeClock clock;
  AdmissionLedger ledger(AdmissionPolicy{}, kPhysical, {}, clock.function());
  start_ocr_and_refuse_asr(ledger);
  const std::uint64_t squeezed = 3 * kGiB;  // 1 GiB above the reserve.
  for (const char* lease : {"o1", "o2", "o3"}) {
    clock.now += 2s;
    ledger.release("s", lease);
  }
  const auto held_back = 6s;
  expect(!ask(ledger, kAsr, "a2", kAsrBytes, squeezed).admitted, "ASR still does not fit");
  expect(!ledger.waiting(kAsr), "nothing it waited for is left: the wait ends");

  expect(ask(ledger, kOcr, "o4", kOcrBytes / 2, squeezed).admitted, "OCR runs");
  expect(ask(ledger, kOcr, "o5", kOcrBytes / 4, squeezed).admitted, "and is not held back");
  clock.now += held_back - 1ms;
  expect(!ask(ledger, kAsr, "a3", kAsrBytes, squeezed).admitted, "ASR refused while resting");
  expect(!ledger.waiting(kAsr), "a resting type does not wait");
  expect(ask(ledger, kOcr, "o6", 0, squeezed).admitted, "OCR still not held back");

  clock.now += 1ms;
  expect(!ask(ledger, kAsr, "a4", kAsrBytes, squeezed).admitted, "rest over; still no room");
  expect(ledger.waiting(kAsr), "ASR waits again while OCR runs");
}

void test_wait_is_bounded_by_the_max_wait() {
  // A lease ASR waits for never ends (stuck). ASR keeps asking within its
  // idle window; the wait still ends at the max wait.
  FakeClock clock;
  const MemoryTurnPolicy turns{.idle_window = 10s, .max_wait = 30s};
  AdmissionLedger ledger(AdmissionPolicy{}, kPhysical, turns, clock.function());
  start_ocr_and_refuse_asr(ledger);
  ledger.release("s", "o2");
  ledger.release("s", "o3");
  // o1 still runs; 3 GiB free, but other work leaves only 1.5 GiB for leases.
  const std::uint64_t squeezed = 4 * kGiB + 512 * kMiB;
  for (auto waited = 0s; waited < turns.max_wait; waited += 2s) {
    clock.now += 2s;
    (void)ask(ledger, kAsr, "a-retry", kAsrBytes, squeezed);
    if (clock.now - std::chrono::steady_clock::time_point{} < turns.max_wait) {
      expect(ledger.waiting(kAsr), "ASR waits while o1 runs, up to the max wait");
      expect(!ask(ledger, kOcr, "o-held", 0, squeezed).admitted, "OCR yields meanwhile");
    }
  }
  expect(!ledger.waiting(kAsr), "the max wait ended the wait");
  expect(ask(ledger, kOcr, "o4", 0, squeezed).admitted, "OCR is no longer held back");
  expect(!ask(ledger, kAsr, "a2", kAsrBytes, squeezed).admitted, "ASR still does not fit");
  expect(!ledger.waiting(kAsr), "and rests after a fruitless wait");
}

void test_waiting_types_are_served_in_order() {
  // ASR waits first, then OCR (which holds leases) is refused for memory and
  // waits too. OCR yields to ASR; ASR never yields to OCR.
  MemoryTurns turns;
  const auto now = std::chrono::steady_clock::time_point{};
  const std::set<LeaseKey> running{{"s", "x"}};
  turns.refused(kAsr, running, now);
  turns.refused(kOcr, running, now);
  expect(turns.waiting(kAsr) && turns.waiting(kOcr), "both wait");
  expect(turns.yields_to(kOcr) == std::string(kAsr), "the later waiter yields to the earlier");
  expect(!turns.yields_to(kAsr).has_value(), "the earliest waiter yields to nobody");
  expect(turns.yields_to(kTrack) == std::string(kAsr), "others yield to the earliest");
  turns.admitted(kAsr);
  expect(turns.yields_to(kTrack) == std::string(kOcr), "then to the next");
  expect(!turns.yields_to(kOcr).has_value(), "OCR now has the turn");
}

void test_no_wait_without_other_types_running() {
  MemoryTurns turns;
  turns.refused(kAsr, {}, std::chrono::steady_clock::time_point{});
  expect(!turns.waiting(kAsr), "nothing of another type to hold back: no wait");
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec-worker-memory-turns-tests",
      {
          {"OCR stream yields to waiting ASR", test_ocr_stream_yields_to_waiting_asr},
          {"only types holding leases yield", test_only_types_holding_leases_yield},
          {"wait ends when the waiting type stops asking",
           test_wait_ends_when_the_waiting_type_stops_asking},
          {"fruitless wait ends and rests", test_fruitless_wait_ends_and_rests},
          {"wait is bounded by the max wait", test_wait_is_bounded_by_the_max_wait},
          {"waiting types are served in order", test_waiting_types_are_served_in_order},
          {"no wait without other types running", test_no_wait_without_other_types_running},
      });
}
