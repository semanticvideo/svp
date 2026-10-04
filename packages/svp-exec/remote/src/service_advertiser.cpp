#include "svp/exec/remote/service_advertiser.hpp"

#include "nw_ref.hpp"
#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/remote_listener.hpp"
#include "svp/exec/remote/transport_policy.hpp"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace svp::exec::remote {

struct ServiceAdvertiser::State {
  ServiceAdvertisement advertisement;
  std::function<void(const std::string&)> log;
  detail::SerialQueue queue{"org.svp.exec.remote.advertiser"};
  // Used on `queue` only.
  DNSServiceRef ref = nullptr;
  mutable std::mutex mutex;
  mutable std::condition_variable changed;
  bool stopped = false;
  bool registered = false;
  std::string name;
  std::string error;
  std::uint32_t failures = 0;
  // Retries since the last success.
  std::uint32_t attempt = 0;
  std::weak_ptr<State> self;

  void say(const std::string& line) {
    if (log) {
      log(line);
    }
  }
};

namespace {

using State = ServiceAdvertiser::State;

void register_now(const std::shared_ptr<State>& state);
// By value: the retry block captures it (a block captures a C++ reference as
// a reference).
void schedule_retry(std::shared_ptr<State> state);

void schedule_retry(std::shared_ptr<State> state) {
  std::chrono::milliseconds delay{};
  {
    const std::lock_guard lock(state->mutex);
    if (state->stopped) {
      return;
    }
    delay = advertise_retry_delay(++state->attempt);
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                               static_cast<std::int64_t>(delay.count()) * NSEC_PER_MSEC),
                 state->queue.get(), ^{
                   register_now(state);
                 });
}

void fail(const std::shared_ptr<State>& state, const std::string& error) {
  if (state->ref != nullptr) {
    DNSServiceRefDeallocate(state->ref);
    state->ref = nullptr;
  }
  {
    const std::lock_guard lock(state->mutex);
    state->registered = false;
    state->error = error;
    ++state->failures;
  }
  state->changed.notify_all();
  state->say("Bonjour registration of `" + state->advertisement.name + "` failed (" + error +
             "); retrying");
  schedule_retry(state);
}

void DNSSD_API on_registered(DNSServiceRef, DNSServiceFlags flags, DNSServiceErrorType error,
                             const char* name, const char*, const char*, void* context) {
  // The context is the State: the advertiser deallocates the reference on
  // this queue before it lets the State go, so no callback outlives it.
  const std::shared_ptr<State> state = static_cast<State*>(context)->self.lock();
  if (!state) {
    return;
  }
  if (error != kDNSServiceErr_NoError) {
    fail(state, "dns_sd error " + std::to_string(error));
    return;
  }
  {
    const std::lock_guard lock(state->mutex);
    state->registered = (flags & kDNSServiceFlagsAdd) != 0;
    state->name = name != nullptr ? name : "";
    state->error.clear();
    state->attempt = 0;
  }
  state->changed.notify_all();
}

void register_now(const std::shared_ptr<State>& state) {
  {
    const std::lock_guard lock(state->mutex);
    if (state->stopped) {
      return;
    }
  }
  TXTRecordRef txt;
  TXTRecordCreate(&txt, 0, nullptr);
  for (const auto& [key, value] : state->advertisement.txt) {
    TXTRecordSetValue(&txt, key.c_str(), static_cast<std::uint8_t>(value.size()), value.data());
  }
  const std::string type(kWorkerServiceType);
  const std::string domain(kWorkerServiceDomain);
  DNSServiceRef ref = nullptr;
  const DNSServiceErrorType error = DNSServiceRegister(
      &ref, 0, kDNSServiceInterfaceIndexAny,
      state->advertisement.name.empty() ? nullptr : state->advertisement.name.c_str(),
      type.c_str(), domain.c_str(), nullptr, htons(state->advertisement.port),
      TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), on_registered, state.get());
  TXTRecordDeallocate(&txt);
  if (error != kDNSServiceErr_NoError) {
    fail(state, "dns_sd error " + std::to_string(error));
    return;
  }
  state->ref = ref;
  const DNSServiceErrorType queued = DNSServiceSetDispatchQueue(ref, state->queue.get());
  if (queued != kDNSServiceErr_NoError) {
    fail(state, "dns_sd error " + std::to_string(queued));
  }
}

}  // namespace

std::chrono::milliseconds advertise_retry_delay(std::uint32_t attempt) noexcept {
  std::chrono::milliseconds delay = kAdvertiseRetryInitial;
  for (std::uint32_t step = 1; step < attempt && delay < kAdvertiseRetryMax; ++step) {
    delay *= 2;
  }
  return std::min(delay, kAdvertiseRetryMax);
}

ServiceAdvertiser::ServiceAdvertiser(ServiceAdvertisement advertisement,
                                     std::function<void(const std::string&)> log)
    : advertisement_(std::move(advertisement)), state_(std::make_shared<State>()) {
  if (advertisement_.port == 0) {
    throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                               "an advertisement needs a port");
  }
  for (const auto& [key, value] : advertisement_.txt) {
    if (key.empty() || key.find('=') != std::string::npos ||
        key.size() + 1 + value.size() > kMaxTxtEntryBytes) {
      throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                                 "TXT entry `" + key + "` is not valid");
    }
  }
  state_->advertisement = advertisement_;
  state_->self = state_;
  state_->log = std::move(log);
  const std::shared_ptr<State> state = state_;
  dispatch_async(state->queue.get(), ^{
    register_now(state);
  });
}

ServiceAdvertiser::~ServiceAdvertiser() {
  const std::shared_ptr<State> state = state_;
  {
    const std::lock_guard lock(state->mutex);
    state->stopped = true;
  }
  // Deregisters on the queue every dns_sd call for it runs on, and waits.
  dispatch_sync(state->queue.get(), ^{
    if (state->ref != nullptr) {
      DNSServiceRefDeallocate(state->ref);
      state->ref = nullptr;
    }
  });
}

bool ServiceAdvertiser::registered() const {
  const std::lock_guard lock(state_->mutex);
  return state_->registered;
}

std::string ServiceAdvertiser::registered_name() const {
  const std::lock_guard lock(state_->mutex);
  return state_->name;
}

std::string ServiceAdvertiser::last_error() const {
  const std::lock_guard lock(state_->mutex);
  return state_->error;
}

std::uint32_t ServiceAdvertiser::failures() const {
  const std::lock_guard lock(state_->mutex);
  return state_->failures;
}

const ServiceAdvertisement& ServiceAdvertiser::advertisement() const noexcept {
  return advertisement_;
}

bool ServiceAdvertiser::wait_registered(std::chrono::milliseconds timeout) const {
  std::unique_lock lock(state_->mutex);
  return state_->changed.wait_for(lock, timeout, [&] { return state_->registered; });
}

}  // namespace svp::exec::remote
