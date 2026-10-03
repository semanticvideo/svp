#include "bonjour_browser.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/transport_policy.hpp"
#include "tls_psk_parameters.hpp"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include <map>

namespace svp::exec::remote::detail {
namespace {

struct BrowseState {
  std::shared_ptr<WaitSignal> signal;
  // Whether a service's TXT entries make it a match.
  std::function<bool(const std::map<std::string, std::string>&)> matches_txt;
  // Settle after the last match (browse every service) instead of the first.
  bool settle_from_last_match = false;
  // Guarded by signal->mutex.
  std::map<std::string, DiscoveredService> matches;
  std::chrono::steady_clock::time_point first_match{};
  std::chrono::steady_clock::time_point last_match{};
  std::string failure;
};

std::map<std::string, std::string> txt_entries(nw_browse_result_t result) {
  std::map<std::string, std::string> entries;
  nw_txt_record_t txt = nw_browse_result_copy_txt_record_object(result);
  if (txt == nullptr) {
    return entries;
  }
  std::map<std::string, std::string>* out = &entries;
  nw_txt_record_apply(txt, ^bool(const char* key, const nw_txt_record_find_key_t found,
                                 const uint8_t* value, const size_t value_length) {
    if (key != nullptr && found == nw_txt_record_find_key_non_empty_value) {
      out->insert_or_assign(key, std::string(reinterpret_cast<const char*>(value), value_length));
    }
    return true;
  });
  nw_release(txt);
  return entries;
}

std::string text_or_empty(const char* text) { return text != nullptr ? text : ""; }

std::string service_name(nw_browse_result_t result) {
  nw_endpoint_t endpoint = nw_browse_result_copy_endpoint(result);
  std::string name = text_or_empty(nw_endpoint_get_bonjour_service_name(endpoint));
  nw_release(endpoint);
  return name;
}

void on_results_changed(const std::shared_ptr<BrowseState>& state, nw_browse_result_t old_result,
                        nw_browse_result_t new_result) {
  {
    const std::lock_guard lock(state->signal->mutex);
    if (old_result != nullptr) {
      state->matches.erase(service_name(old_result));
    }
    std::map<std::string, std::string> txt =
        new_result != nullptr ? txt_entries(new_result) : std::map<std::string, std::string>{};
    if (new_result != nullptr && state->matches_txt(txt)) {
      DiscoveredService service;
      service.txt = std::move(txt);
      const auto endpoint =
          NwRef<nw_endpoint_t>::adopt(nw_browse_result_copy_endpoint(new_result));
      service.name = text_or_empty(nw_endpoint_get_bonjour_service_name(endpoint.get()));
      service.type = text_or_empty(nw_endpoint_get_bonjour_service_type(endpoint.get()));
      service.domain = text_or_empty(nw_endpoint_get_bonjour_service_domain(endpoint.get()));
      std::vector<NwRef<nw_interface_t>>* interfaces = &service.interfaces;
      nw_browse_result_enumerate_interfaces(new_result, ^bool(nw_interface_t interface) {
        interfaces->push_back(NwRef<nw_interface_t>::retain(interface));
        return true;
      });
      if (state->matches.empty() &&
          state->first_match == std::chrono::steady_clock::time_point{}) {
        state->first_match = std::chrono::steady_clock::now();
      }
      state->last_match = std::chrono::steady_clock::now();
      state->matches.insert_or_assign(service.name, std::move(service));
    }
  }
  state->signal->changed.notify_all();
}

}  // namespace

RouteMedium route_medium_of(nw_interface_t interface) {
  switch (nw_interface_get_type(interface)) {
    case nw_interface_type_loopback:
      return RouteMedium::loopback;
    case nw_interface_type_wired:
      return RouteMedium::wired;
    case nw_interface_type_wifi:
      return RouteMedium::wifi;
    case nw_interface_type_cellular:
      return RouteMedium::cellular;
    default:
      return RouteMedium::other;
  }
}

namespace {

std::vector<DiscoveredService> browse(std::shared_ptr<BrowseState> state,
                                      const RoutePolicy& policy,
                                      const std::shared_ptr<WaitSignal>& signal) {
  // `state` is held by value: the browser's blocks capture it, and a block
  // captures a C++ reference as a reference.
  const std::string type(kWorkerServiceType);
  const std::string domain(kWorkerServiceDomain);
  auto descriptor = NwRef<nw_browse_descriptor_t>::adopt(
      nw_browse_descriptor_create_bonjour_service(type.c_str(), domain.c_str()));
  nw_browse_descriptor_set_include_txt_record(descriptor.get(), true);
  auto parameters = NwRef<nw_parameters_t>::adopt(nw_parameters_create());
  nw_parameters_set_include_peer_to_peer(parameters.get(), false);
  auto browser =
      NwRef<nw_browser_t>::adopt(nw_browser_create(descriptor.get(), parameters.get()));
  SerialQueue queue("org.svp.exec.remote.browser");
  nw_browser_set_queue(browser.get(), queue.get());
  nw_browser_set_browse_results_changed_handler(
      browser.get(), ^(nw_browse_result_t old_result, nw_browse_result_t new_result, bool) {
        on_results_changed(state, old_result, new_result);
      });
  nw_browser_set_state_changed_handler(browser.get(), ^(nw_browser_state_t browser_state,
                                                        nw_error_t error) {
    if (browser_state == nw_browser_state_failed) {
      {
        const std::lock_guard lock(state->signal->mutex);
        state->failure = describe_nw_error(error);
      }
      state->signal->changed.notify_all();
    }
  });
  nw_browser_start(browser.get());

  const auto deadline = std::chrono::steady_clock::now() + policy.discovery_timeout;
  std::vector<DiscoveredService> found;
  bool cancelled = false;
  std::string failure;
  {
    std::unique_lock lock(signal->mutex);
    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (signal->cancelled) {
        cancelled = true;
        break;
      }
      if (!state->failure.empty()) {
        failure = state->failure;
        break;
      }
      const bool have_match = !state->matches.empty();
      const auto settle_end =
          (state->settle_from_last_match ? state->last_match : state->first_match) +
          policy.discovery_settle;
      if ((have_match && now >= settle_end) || now >= deadline) {
        break;
      }
      signal->changed.wait_until(lock, have_match ? std::min(settle_end, deadline) : deadline);
    }
    for (auto& [name, service] : state->matches) {
      found.push_back(std::move(service));
    }
    state->matches.clear();
  }
  nw_browser_cancel(browser.get());
  if (cancelled) {
    throw RemoteTransportError(RemoteErrorCode::cancelled, "discovery was cancelled");
  }
  if (!failure.empty()) {
    throw RemoteTransportError(RemoteErrorCode::worker_not_found,
                               "Bonjour browsing failed: " + failure);
  }
  return found;
}

}  // namespace

std::vector<DiscoveredService> browse_for_pairing(std::string_view pairing_id,
                                                  const RoutePolicy& policy,
                                                  const std::shared_ptr<WaitSignal>& signal) {
  const auto state = std::make_shared<BrowseState>();
  state->signal = signal;
  state->matches_txt = [wanted = std::string(pairing_id)](const auto& txt) {
    const auto found = txt.find(std::string(kPairingTxtKey));
    return found != txt.end() && found->second == wanted;
  };
  return browse(state, policy, signal);
}

std::vector<DiscoveredService> browse_for_txt(std::string_view key, std::string_view value,
                                              const RoutePolicy& policy,
                                              const std::shared_ptr<WaitSignal>& signal) {
  const auto state = std::make_shared<BrowseState>();
  state->signal = signal;
  state->matches_txt = [wanted_key = std::string(key), wanted = std::string(value)](
                           const auto& txt) {
    const auto found = txt.find(wanted_key);
    return found != txt.end() && found->second == wanted;
  };
  state->settle_from_last_match = true;
  return browse(state, policy, signal);
}

std::vector<DiscoveredService> browse_for_worker(std::string_view worker_id,
                                                 std::string_view pairing_id,
                                                 const RoutePolicy& policy,
                                                 const std::shared_ptr<WaitSignal>& signal) {
  const auto state = std::make_shared<BrowseState>();
  state->signal = signal;
  state->matches_txt = [worker = std::string(worker_id),
                        pairing = std::string(pairing_id)](const auto& txt) {
    const auto by_worker = txt.find(std::string(kWorkerTxtKey));
    const auto by_pairing = txt.find(std::string(kPairingTxtKey));
    return (by_worker != txt.end() && by_worker->second == worker) ||
           (by_pairing != txt.end() && by_pairing->second == pairing);
  };
  std::vector<DiscoveredService> found = browse(state, policy, signal);
  // One worker may advertise itself and, for older coordinators, one
  // instance per pairing, all on the same port: keep one instance, its
  // own advertisement when it is there.
  const auto is_worker = [&](const DiscoveredService& service) {
    const auto entry = service.txt.find(std::string(kWorkerTxtKey));
    return entry != service.txt.end() && entry->second == worker_id;
  };
  const auto own = std::find_if(found.begin(), found.end(), [&](const DiscoveredService& service) {
    return is_worker(service) && !service.txt.contains(std::string(kPairingTxtKey));
  });
  const auto any = own != found.end() ? own : std::find_if(found.begin(), found.end(), is_worker);
  if (any != found.end()) {
    DiscoveredService chosen = std::move(*any);
    found.clear();
    found.push_back(std::move(chosen));
  }
  return found;
}

}  // namespace svp::exec::remote::detail
