// Slot sharing across coordinators (M6) and this Mac's own build's load
// file: one coordinator alone is never limited; several share each type's
// declared slots, fewest-held and longest-waiting first; this Mac's own
// running tasks take slots first; a coordinator that stops asking stops
// holding a slot back.

#include "svp/exec/worker/local_load.hpp"
#include "svp/exec/worker/slot_sharing.hpp"
#include "worker_test_support.hpp"

#include <unistd.h>

namespace {

using namespace svp::exec::worker;
using namespace svp::exec::worker::test;
using namespace std::chrono_literals;

constexpr std::string_view kType = "ocr.frame_batch";
constexpr std::string_view kOtherType = "track.window";

struct FakeClock {
  std::chrono::steady_clock::time_point now{};
  SlotSharing::Clock function() {
    return [this] { return now; };
  }
};

SlotRequest request(std::string coordinator, std::string lease, std::uint64_t slots,
                    std::string_view type = kType) {
  return SlotRequest{.coordinator = coordinator,
                     .session = "session-" + coordinator,
                     .lease_id = std::move(lease),
                     .task_type = std::string(type),
                     .declared_slots = slots};
}

void test_one_coordinator_is_never_limited() {
  SlotSharing slots;
  for (int lease = 0; lease < 5; ++lease) {
    expect(slots.try_take(request("a", "l" + std::to_string(lease), 2)).admitted,
           "a coordinator alone is admitted beyond its declared slots, as before M6");
  }
  expect(slots.held(kType) == 5, "every lease is held");
}

void test_undeclared_sessions_are_not_limited() {
  SlotSharing slots;
  expect(slots.try_take(request("a", "l1", 1)).admitted, "a takes its slot");
  expect(slots.try_take(request("b", "l1", 0)).admitted,
         "a session that declared no slots is admitted by memory alone");
  expect(slots.held(kType) == 2, "and counts as held");
}

void test_two_coordinators_share_the_declared_slots() {
  FakeClock clock;
  SlotSharing slots(10s, {}, clock.function());
  expect(slots.try_take(request("a", "a1", 2)).admitted, "a: first slot");
  expect(slots.try_take(request("b", "b1", 2)).admitted, "b: second slot");
  const AdmissionDecision full = slots.try_take(request("a", "a2", 2));
  expect(!full.admitted && full.code == kRejectInsufficientSlots, "both slots in use");
  expect(full.message.find("other coordinators") != std::string::npos, "message explains why");
  expect(!slots.try_take(request("b", "b2", 2)).admitted, "b is refused too");
  expect(slots.try_take(request("c", "c1", 0)).admitted, "undeclared sessions still pass");
  slots.release("session-c", "c1");
  slots.release("session-a", "a1");
  // a and b both wait; a started waiting first and holds fewer.
  expect(!slots.try_take(request("b", "b3", 2)).admitted,
         "the freed slot is kept for a, which holds fewer");
  expect(slots.try_take(request("a", "a3", 2)).admitted, "a gets it");
}

void test_a_finished_lease_does_not_jump_the_queue() {
  FakeClock clock;
  SlotSharing slots(10s, {}, clock.function());
  expect(slots.try_take(request("a", "a1", 1)).admitted, "a holds the only slot");
  expect(!slots.try_take(request("b", "b1", 1)).admitted, "b waits");
  slots.release("session-a", "a1");
  clock.now += 100ms;
  expect(!slots.try_take(request("a", "a2", 1)).admitted,
         "a, whose lease just finished, does not take the slot b waits for");
  clock.now += 1s;
  expect(slots.try_take(request("b", "b2", 1)).admitted, "b gets it on its next try");
  slots.release("session-b", "b2");
  clock.now += 100ms;
  expect(!slots.try_take(request("b", "b3", 1)).admitted, "now a's turn: it has been waiting");
  expect(slots.try_take(request("a", "a3", 1)).admitted, "a gets it");
}

void test_a_coordinator_that_stops_asking_stops_waiting() {
  FakeClock clock;
  SlotSharing slots(10s, {}, clock.function());
  expect(slots.try_take(request("a", "a1", 1)).admitted, "a holds the slot");
  expect(!slots.try_take(request("b", "b1", 1)).admitted, "b waits");
  slots.release("session-a", "a1");
  clock.now += 11s;
  expect(slots.try_take(request("a", "a2", 1)).admitted,
         "b did not ask again within the window: the slot is not held back");
}

void test_types_are_shared_separately() {
  SlotSharing slots;
  expect(slots.try_take(request("a", "a1", 1)).admitted, "a: OCR");
  expect(slots.try_take(request("b", "b1", 1, kOtherType)).admitted,
         "b: tracking, another type with slots of its own");
}

void test_local_build_takes_slots_first() {
  TaskTypeCounts local{{std::string(kType), 1}};
  SlotSharing slots(10s, [&local] { return local; });
  const AdmissionDecision refused = slots.try_take(request("a", "a1", 1));
  expect(!refused.admitted && refused.message.find("own build") != std::string::npos,
         "this Mac's own build holds the only slot");
  expect(slots.try_take(request("a", "a2", 2)).admitted,
         "with two declared slots, one is left for the coordinator");
  local.clear();
  expect(slots.try_take(request("a", "a3", 1)).admitted,
         "once the local task ends a coordinator alone is unlimited again");
}

void test_session_release_frees_every_lease() {
  SlotSharing slots;
  expect(slots.try_take(request("a", "a1", 1)).admitted, "a1");
  expect(slots.try_take(request("a", "a2", 1)).admitted, "a2");
  slots.release_session("session-a");
  expect(slots.held(kType) == 0, "a lost session holds nothing");
}

void test_local_load_files_are_summed_while_their_process_lives() {
  TemporaryDirectory dir("svp-local-load");
  const std::filesystem::path directory = dir.path / "LocalLoad";
  expect(read_local_load(directory).empty(), "no directory, no load");
  {
    LocalLoadRecorder recorder(directory);
    recorder.add(kType);
    recorder.add(kType);
    recorder.add(kCoordinatingTaskType);
    recorder.remove(kType);
    const TaskTypeCounts load = read_local_load(directory);
    expect(load.at(std::string(kType)) == 1 && load.at(std::string(kCoordinatingTaskType)) == 1,
           "this process's counts are read");
    recorder.remove(kOtherType);
    expect(read_local_load(directory).size() == 2, "removing an unknown type changes nothing");
  }
  expect(read_local_load(directory).empty(), "the file goes with the recorder");

  // A file of a process that is no longer alive: a child that exited.
  const pid_t child = ::fork();
  if (child == 0) {
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  const std::filesystem::path stale = directory / (std::to_string(child) + ".json");
  write_file(stale, R"({"pid":)" + std::to_string(child) +
                        R"(,"running":{"ocr.frame_batch":3},"schema":"svp.local-load/1"})");
  write_file(directory / "broken.json", "not json");
  expect(read_local_load(directory).empty(), "dead and malformed files are ignored");
  {
    LocalLoadRecorder recorder(directory);
    expect(!std::filesystem::exists(stale), "the next build removes a dead build's file");
  }
}

}  // namespace

int main() {
  return run_tests(
      "svp-exec-worker-slot-sharing-tests",
      {
          {"one coordinator is never limited", test_one_coordinator_is_never_limited},
          {"undeclared sessions are not limited", test_undeclared_sessions_are_not_limited},
          {"two coordinators share the declared slots",
           test_two_coordinators_share_the_declared_slots},
          {"a finished lease does not jump the queue",
           test_a_finished_lease_does_not_jump_the_queue},
          {"a coordinator that stops asking stops waiting",
           test_a_coordinator_that_stops_asking_stops_waiting},
          {"types are shared separately", test_types_are_shared_separately},
          {"the local build takes slots first", test_local_build_takes_slots_first},
          {"session release frees every lease", test_session_release_frees_every_lease},
          {"local load files are summed while their process lives",
           test_local_load_files_are_summed_while_their_process_lives},
      });
}
