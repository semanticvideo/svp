#include "interface_link_rate.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>
#include <sys/socket.h>

#include <string>

namespace svp::exec::remote::detail {

std::uint64_t interface_link_rate_bps(std::string_view interface_name) {
  ifaddrs* addresses = nullptr;
  if (interface_name.empty() || ::getifaddrs(&addresses) != 0) {
    return 0;
  }
  std::uint64_t rate = 0;
  const std::string name(interface_name);
  for (const ifaddrs* entry = addresses; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_LINK &&
        entry->ifa_data != nullptr && name == entry->ifa_name) {
      rate = static_cast<const if_data*>(entry->ifa_data)->ifi_baudrate;
      break;
    }
  }
  ::freeifaddrs(addresses);
  return rate;
}

}  // namespace svp::exec::remote::detail
