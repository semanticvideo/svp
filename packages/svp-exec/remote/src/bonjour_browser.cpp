#include "bonjour_browser.hpp"

#include "svp/exec/remote/remote_error.hpp"
#include "svp/exec/remote/transport_policy.hpp"
#include "tls_psk_parameters.hpp"

#include <chrono>
#include <cstring>
#include <map>

namespace svp::exec::remote::detail {
namespace {

struct BrowseState {
  std::shared_ptr<WaitSignal> signal;
  std::string pairing_id;
  // Guarded by signal->mutex.
  std::map<std::string, DiscoveredService> matches;
  std::chrono::steady_clock::time_point first_match{};
  std::string failure;
};

bool advertises_pairing(nw_browse_result_t result, const std::string& pairing_id) {
  nw_txt_record_t txt = nw_browse_result_copy_txt_record_object(result);
  if (txt == nullptr) {
    return false;
  }
  bool matches = false;
  bool* out = &matches;
  const std::string key(kPairingTxtKey);
  nw_txt_record_access_key(txt, key.c_str(),
                           ^bool(const char*, const nw_txt_record_find_key_t found,
                                 const uint8_t* value, const size_t value_length) {
                             *out = found == nw_txt_record_find_key_non_empty_value &&
                                    value_length == pairing_id.size() &&
                                    std::memcmp(value, pairing_id.data(), value_length) == 0;
                             return true;
                           });
  nw_release(txt);
  return matches;
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
    if (new_result != nullptr && advertises_pairing(new_result, state->pairing_id)) {
      DiscoveredService service;
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

std::vector<DiscoveredService> browse_for_pairing(std::string_view pairing_id,
                                                  const RoutePolicy& policy,
                                                  const std::shared_ptr<WaitSignal>& signal) {
  const auto state = std::make_shared<BrowseState>();
  state->signal = signal;
  state->pairing_id = std::string(pairing_id);

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
      const auto settle_end = state->first_match + policy.discovery_settle;
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

}  // namespace svp::exec::remote::detail
