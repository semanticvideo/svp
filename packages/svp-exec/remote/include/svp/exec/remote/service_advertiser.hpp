#pragma once

// One Bonjour (DNS-SD) registration of kWorkerServiceType on a port, made
// with the system dns_sd API apart from any listener, so a registration that
// is slow or fails never stops a listener from serving (plan §3.4).
//
// A registration that fails (mDNSResponder restarting, the network not up
// yet at boot, a conflict it cannot resolve) is made again after a backoff:
// kAdvertiseRetryInitial, doubling up to kAdvertiseRetryMax, and back to the
// initial delay once a registration succeeds. Nothing is thrown for it; the
// state is reported (registered(), last_error()) and logged through `log`.
// Name conflicts are resolved by the system (it renames the instance);
// callers pick names that do not collide in the first place.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace svp::exec::remote {

// A first retry soon enough that a registration refused while the system
// was still starting succeeds within a second of it being able to.
inline constexpr std::chrono::milliseconds kAdvertiseRetryInitial{1'000};
// At most one attempt a minute while registration keeps failing, so a broken
// mDNSResponder does not flood the log.
inline constexpr std::chrono::milliseconds kAdvertiseRetryMax{60'000};

// The delay before retry `attempt` (1 for the first retry).
[[nodiscard]] std::chrono::milliseconds advertise_retry_delay(std::uint32_t attempt) noexcept;

struct ServiceAdvertisement {
  // Empty: this Mac's computer name.
  std::string name;
  std::uint16_t port = 0;
  // Each entry within kMaxTxtEntryBytes (remote_listener.hpp).
  std::map<std::string, std::string> txt;
};

class ServiceAdvertiser {
 public:
  // Starts registering at once. Throws RemoteTransportError
  // (invalid_configuration) for a port of 0 or an oversized TXT entry.
  ServiceAdvertiser(ServiceAdvertisement advertisement,
                    std::function<void(const std::string&)> log = {});
  ~ServiceAdvertiser();
  ServiceAdvertiser(const ServiceAdvertiser&) = delete;
  ServiceAdvertiser& operator=(const ServiceAdvertiser&) = delete;

  [[nodiscard]] bool registered() const;
  // The instance name the system registered (after any rename).
  [[nodiscard]] std::string registered_name() const;
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] std::uint32_t failures() const;
  [[nodiscard]] const ServiceAdvertisement& advertisement() const noexcept;

  // Blocks until registered or `timeout` passes; true when registered.
  bool wait_registered(std::chrono::milliseconds timeout) const;

  struct State;

 private:
  ServiceAdvertisement advertisement_;
  std::shared_ptr<State> state_;
};

}  // namespace svp::exec::remote
