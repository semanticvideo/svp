#pragma once

// POSIX socket helpers for the transport and worker tests (Apple only, like
// the code they test).

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <stdexcept>

namespace svp::exec::test {

// A TCP port free right now: one the system chose for a socket that is
// closed again before this returns.
inline std::uint16_t free_tcp_port() {
  const int socket = ::socket(AF_INET6, SOCK_STREAM, 0);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  socklen_t length = sizeof(address);
  const bool bound =
      socket >= 0 && ::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
      ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0;
  if (socket >= 0) {
    ::close(socket);
  }
  if (!bound) {
    throw std::runtime_error("no free TCP port");
  }
  return ntohs(address.sin6_port);
}

}  // namespace svp::exec::test
