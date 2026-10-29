#include "svp/exec/worker/idle_sleep_assertion.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace svp::exec::worker {

IdleSleepAssertion::IdleSleepAssertion(const std::string& name) {
  CFStringRef reason =
      CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
  if (reason == nullptr) {
    return;
  }
  IOPMAssertionID id = kIOPMNullAssertionID;
  held_ = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                      kIOPMAssertionLevelOn, reason, &id) == kIOReturnSuccess;
  id_ = id;
  CFRelease(reason);
}

IdleSleepAssertion::~IdleSleepAssertion() {
  if (held_) {
    (void)IOPMAssertionRelease(id_);
  }
}

}  // namespace svp::exec::worker
