#include "service_resolver.hpp"

#include "nw_ref.hpp"
#include "svp/exec/remote/remote_error.hpp"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netdb.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstring>
#include <list>

namespace svp::exec::remote::detail {
namespace {

struct Resolution {
  ServiceOnInterface request;
  std::shared_ptr<WaitSignal> signal;
  dispatch_queue_t queue = nullptr;
  DNSServiceRef resolve_ref = nullptr;
  DNSServiceRef address_ref = nullptr;
  // Guarded by signal->mutex.
  std::uint16_t port = 0;
  std::vector<ResolvedAddress> addresses;
  bool have_ipv4 = false;
  bool have_ipv6 = false;
  std::chrono::steady_clock::time_point first_address{};
  bool failed = false;

  [[nodiscard]] bool settled(std::chrono::steady_clock::time_point now,
                             std::chrono::milliseconds settle) const {
    return failed || (have_ipv4 && have_ipv6) ||
           (!addresses.empty() && now >= first_address + settle);
  }
};

std::string numeric_text(const sockaddr_storage& address) {
  char host[NI_MAXHOST] = {};
  char service[NI_MAXSERV] = {};
  if (::getnameinfo(reinterpret_cast<const sockaddr*>(&address), address.ss_len, host,
                    sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "?";
  }
  return std::string(host) + " port " + service;
}

void DNSSD_API on_address(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType error,
                          const char*, const sockaddr* address, uint32_t, void* context) {
  auto* resolution = static_cast<Resolution*>(context);
  {
    const std::lock_guard lock(resolution->signal->mutex);
    if (error == kDNSServiceErr_NoSuchRecord && address != nullptr) {
      // A definite "no address of this family" counts as that family's answer.
      (address->sa_family == AF_INET ? resolution->have_ipv4 : resolution->have_ipv6) = true;
    } else if (error != kDNSServiceErr_NoError) {
      resolution->failed = resolution->addresses.empty();
    } else if ((flags & kDNSServiceFlagsAdd) != 0 && address != nullptr &&
               (address->sa_family == AF_INET || address->sa_family == AF_INET6)) {
      ResolvedAddress resolved;
      std::memcpy(&resolved.address, address, address->sa_len);
      if (address->sa_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&resolved.address)->sin_port = htons(resolution->port);
        resolution->have_ipv4 = true;
      } else {
        auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&resolved.address);
        ipv6->sin6_port = htons(resolution->port);
        if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr) && ipv6->sin6_scope_id == 0) {
          ipv6->sin6_scope_id = resolution->request.interface_index;
        }
        resolution->have_ipv6 = true;
      }
      resolved.text = numeric_text(resolved.address);
      const bool duplicate =
          std::any_of(resolution->addresses.begin(), resolution->addresses.end(),
                      [&](const ResolvedAddress& known) { return known.text == resolved.text; });
      if (!duplicate) {
        if (resolution->addresses.empty()) {
          resolution->first_address = std::chrono::steady_clock::now();
        }
        resolution->addresses.push_back(std::move(resolved));
      }
    }
  }
  resolution->signal->changed.notify_all();
}

void DNSSD_API on_resolved(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error,
                           const char*, const char* host_target, uint16_t port, uint16_t,
                           const unsigned char*, void* context) {
  auto* resolution = static_cast<Resolution*>(context);
  bool start_lookup = false;
  {
    const std::lock_guard lock(resolution->signal->mutex);
    if (error != kDNSServiceErr_NoError) {
      resolution->failed = true;
    } else if (resolution->address_ref == nullptr) {
      resolution->port = ntohs(port);
      start_lookup = true;
    }
  }
  if (start_lookup) {
    // Runs on the resolution queue, where every DNSServiceRef is used.
    DNSServiceRef address_ref = nullptr;
    const DNSServiceErrorType status = DNSServiceGetAddrInfo(
        &address_ref, 0, resolution->request.interface_index,
        kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6, host_target, on_address, resolution);
    if (status == kDNSServiceErr_NoError) {
      DNSServiceSetDispatchQueue(address_ref, resolution->queue);
    }
    const std::lock_guard lock(resolution->signal->mutex);
    resolution->address_ref = status == kDNSServiceErr_NoError ? address_ref : nullptr;
    resolution->failed = resolution->failed || status != kDNSServiceErr_NoError;
  }
  resolution->signal->changed.notify_all();
}

}  // namespace

std::vector<std::vector<ResolvedAddress>> resolve_services(
    const std::vector<ServiceOnInterface>& requests, std::chrono::milliseconds timeout,
    std::chrono::milliseconds settle, const std::shared_ptr<WaitSignal>& signal) {
  SerialQueue queue("org.svp.exec.remote.resolver");
  // std::list: callbacks hold pointers to the elements.
  std::list<Resolution> resolutions;
  for (const ServiceOnInterface& request : requests) {
    Resolution& resolution = resolutions.emplace_back();
    resolution.request = request;
    resolution.signal = signal;
    resolution.queue = queue.get();
  }
  // Blocks copy captured C++ objects; capture the list by pointer.
  std::list<Resolution>* pending = &resolutions;
  dispatch_sync(queue.get(), ^{
    for (Resolution& resolution : *pending) {
      const DNSServiceErrorType status = DNSServiceResolve(
          &resolution.resolve_ref, 0, resolution.request.interface_index,
          resolution.request.name.c_str(), resolution.request.type.c_str(),
          resolution.request.domain.c_str(), on_resolved, &resolution);
      if (status == kDNSServiceErr_NoError) {
        DNSServiceSetDispatchQueue(resolution.resolve_ref, resolution.queue);
      } else {
        const std::lock_guard lock(signal->mutex);
        resolution.resolve_ref = nullptr;
        resolution.failed = true;
      }
    }
  });

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool cancelled = false;
  std::vector<std::vector<ResolvedAddress>> results;
  {
    std::unique_lock lock(signal->mutex);
    while (true) {
      const auto now = std::chrono::steady_clock::now();
      cancelled = signal->cancelled;
      const bool all_settled =
          std::all_of(resolutions.begin(), resolutions.end(),
                      [&](const Resolution& resolution) { return resolution.settled(now, settle); });
      if (cancelled || all_settled || now >= deadline) {
        break;
      }
      auto wake = deadline;
      for (const Resolution& resolution : resolutions) {
        if (!resolution.addresses.empty()) {
          wake = std::min(wake, resolution.first_address + settle);
        }
      }
      signal->changed.wait_until(lock, wake);
    }
    for (Resolution& resolution : resolutions) {
      results.push_back(std::move(resolution.addresses));
    }
  }
  // DNSServiceRefDeallocate must run on the queue the refs were scheduled on;
  // after it returns no callback touches `resolutions` again.
  dispatch_sync(queue.get(), ^{
    for (Resolution& resolution : *pending) {
      if (resolution.address_ref != nullptr) {
        DNSServiceRefDeallocate(resolution.address_ref);
      }
      if (resolution.resolve_ref != nullptr) {
        DNSServiceRefDeallocate(resolution.resolve_ref);
      }
    }
  });
  if (cancelled) {
    throw RemoteTransportError(RemoteErrorCode::cancelled, "service resolution was cancelled");
  }
  return results;
}

}  // namespace svp::exec::remote::detail
