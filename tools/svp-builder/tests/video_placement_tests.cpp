// svp-builder build --coordinators: this Mac's coordinator slot and where one
// video is coordinated (batch/video_placement.hpp).

#include "batch/coordinator_slot.hpp"
#include "batch/video_placement.hpp"

#include "svp/builder/remote_video_builder.hpp"
#include "svp/exec/cancellation_token.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bt = svp::builder::batch;
using Status = bt::RemoteVideoStatus;
using Where = bt::VideoPlacement::Where;

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

fs::path slot_path(const std::string& name) {
  const fs::path path = fs::temp_directory_path() /
                        ("svp-placement-" + name + "-" + std::to_string(::getpid())) /
                        "slot.lock";
  fs::remove_all(path.parent_path());
  return path;
}

// Another Mac whose answers follow `script` (the last one repeats).
class ScriptedMac final : public bt::RemoteVideoBuilder {
 public:
  ScriptedMac(std::string name, std::vector<Status> script)
      : name_(std::move(name)), script_(std::move(script)) {}
  [[nodiscard]] std::string name() const override { return name_; }
  [[nodiscard]] bt::RemoteVideoOutcome build(const bt::RemoteVideoRequest& request) override {
    last_item = request.item_id;
    const Status status = script_[std::min(calls, script_.size() - 1)];
    ++calls;
    return {.status = status, .message = name_ + " " + std::to_string(calls)};
  }
  std::size_t calls = 0;
  std::string last_item;

 private:
  std::string name_;
  std::vector<Status> script_;
};

bt::VideoPlacementOptions options(const fs::path& slot,
                                  std::vector<std::shared_ptr<bt::RemoteVideoBuilder>> macs) {
  return {.slot_path = slot,
          .coordinators = std::move(macs),
          .request = {.item_id = "video"},
          .busy_backoff = std::chrono::milliseconds(10)};
}

void test_one_build_holds_the_slot() {
  const fs::path slot = slot_path("slot");
  std::optional<bt::CoordinatorSlot> first = bt::CoordinatorSlot::try_take(slot);
  require(first.has_value(), "a free slot is taken");
  require(!bt::CoordinatorSlot::try_take(slot), "a held slot is not taken again");
  first.reset();
  require(bt::CoordinatorSlot::try_take(slot).has_value(), "a released slot is free again");
}

void test_a_free_slot_keeps_the_video_here() {
  const fs::path slot = slot_path("here");
  auto mac = std::make_shared<ScriptedMac>("raphael", std::vector{Status::built});
  bt::VideoPlacement placed = bt::place_video(options(slot, {mac}));
  require(placed.where == Where::here && placed.slot.has_value(), "coordinated here");
  require(mac->calls == 0, "no other Mac is asked while this Mac is free");
  require(!bt::CoordinatorSlot::try_take(slot), "the local build holds the slot");
}

void test_a_busy_mac_is_asked_again_and_the_next_one_takes_it() {
  const fs::path slot = slot_path("busy");
  const std::optional<bt::CoordinatorSlot> other_build = bt::CoordinatorSlot::try_take(slot);
  auto busy = std::make_shared<ScriptedMac>("raphael", std::vector{Status::busy});
  auto later = std::make_shared<ScriptedMac>("april", std::vector{Status::busy, Status::built});
  bt::VideoPlacement placed = bt::place_video(options(slot, {busy, later}));
  require(placed.where == Where::other_mac && placed.mac == "april" &&
              placed.outcome.status == Status::built,
          "the first Mac to take it coordinates it");
  require(busy->calls == 2 && later->calls == 2, "busy Macs are asked again after a pause");
  require(later->last_item == "video", "the request reaches the Mac");
}

void test_a_failed_build_elsewhere_is_reported_not_retried() {
  const fs::path slot = slot_path("failed");
  const std::optional<bt::CoordinatorSlot> other_build = bt::CoordinatorSlot::try_take(slot);
  auto failing = std::make_shared<ScriptedMac>("raphael", std::vector{Status::failed});
  auto spare = std::make_shared<ScriptedMac>("april", std::vector{Status::built});
  bt::VideoPlacement placed = bt::place_video(options(slot, {failing, spare}));
  require(placed.where == Where::other_mac && placed.mac == "raphael" &&
              placed.outcome.status == Status::failed && spare->calls == 0,
          "a build that ran and failed is the video's result");
}

void test_with_no_mac_available_the_video_waits_for_this_mac() {
  const fs::path slot = slot_path("wait");
  std::optional<bt::CoordinatorSlot> other_build = bt::CoordinatorSlot::try_take(slot);
  auto gone = std::make_shared<ScriptedMac>("raphael", std::vector{Status::unavailable});
  std::thread finish([&other_build] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    other_build.reset();
  });
  const auto started = std::chrono::steady_clock::now();
  bt::VideoPlacement placed = bt::place_video(options(slot, {gone}));
  finish.join();
  require(placed.where == Where::here && placed.slot.has_value(),
          "it is coordinated here once the other build ends");
  require(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(300),
          "it waited for the slot");
  require(gone->calls == 1, "an unavailable Mac is not asked again");
}

void test_a_cancelled_build_is_placed_nowhere() {
  const fs::path slot = slot_path("cancel");
  const std::optional<bt::CoordinatorSlot> other_build = bt::CoordinatorSlot::try_take(slot);
  auto busy = std::make_shared<ScriptedMac>("raphael", std::vector{Status::busy});
  svp::exec::CancellationToken cancellation;
  bt::VideoPlacementOptions cancellable = options(slot, {busy});
  cancellable.busy_backoff = std::chrono::milliseconds(2000);
  cancellable.cancellation = &cancellation;
  std::thread cancel([&cancellation] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancellation.request();
  });
  const auto started = std::chrono::steady_clock::now();
  const bt::VideoPlacement placed = bt::place_video(cancellable);
  cancel.join();
  require(placed.where == Where::cancelled, "cancelled");
  require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
          "a cancelled build stops waiting promptly");
}

}  // namespace

int main() {
  test_one_build_holds_the_slot();
  test_a_free_slot_keeps_the_video_here();
  test_a_busy_mac_is_asked_again_and_the_next_one_takes_it();
  test_a_failed_build_elsewhere_is_reported_not_retried();
  test_with_no_mac_available_the_video_waits_for_this_mac();
  test_a_cancelled_build_is_placed_nowhere();
  std::cout << "video placement tests passed\n";
  return 0;
}
