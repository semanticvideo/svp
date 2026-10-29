#include "svp/exec/worker/stay_awake.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace svp::exec::worker {
namespace {

bool create(CFStringRef type, CFStringRef name, std::uint32_t& id) {
  IOPMAssertionID created = kIOPMNullAssertionID;
  const bool held =
      IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn, name, &created) == kIOReturnSuccess;
  id = created;
  return held;
}

}  // namespace

StayAwake::StayAwake(const std::string& name) {
  CFStringRef reason =
      CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
  if (reason == nullptr) {
    return;
  }
  idle_held_ = create(kIOPMAssertionTypePreventUserIdleSystemSleep, reason, idle_id_);
  system_held_ = create(kIOPMAssertionTypePreventSystemSleep, reason, system_id_);
  CFRelease(reason);
}

StayAwake::~StayAwake() {
  if (idle_held_) {
    (void)IOPMAssertionRelease(idle_id_);
  }
  if (system_held_) {
    (void)IOPMAssertionRelease(system_id_);
  }
}

}  // namespace svp::exec::worker
