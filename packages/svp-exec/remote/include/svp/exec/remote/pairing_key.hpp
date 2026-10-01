#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace svp::exec::remote {

// Plan §3.3: pairing writes "a random 256-bit pairing secret" on both sides.
// Shorter secrets are refused so a weak key cannot be configured by mistake.
inline constexpr std::size_t kMinPairingSecretBytes = 32;

// The pairing id travels in the Bonjour TXT record, where one entry
// ("pairing=<id>") must fit in 255 bytes (RFC 6763 §6.1), and as the TLS PSK
// identity. 64 bytes leaves room for a UUID or a BLAKE3 hex prefix with a
// label while staying far inside both limits.
inline constexpr std::size_t kMaxPairingIdBytes = 64;

// Pairing material (plan §3.3, §4.1 `pairing`). How it is created and stored
// belongs to the pairing flow; the transport only consumes it.
//
//   pairing_id  opaque public name of one coordinator-worker pairing,
//               [A-Za-z0-9._-], 1..kMaxPairingIdBytes. Advertised in the
//               worker's TXT record so the coordinator finds the worker by
//               pairing, never by host name or address (plan §3.4), and sent
//               as the TLS PSK identity.
//   secret      the pre-shared key both sides hold; never sent.
struct PairingKey {
  std::string pairing_id;
  std::vector<std::byte> secret;
};

// Throws RemoteTransportError(invalid_configuration) naming the problem.
void validate_pairing_key(const PairingKey& key);

}  // namespace svp::exec::remote
