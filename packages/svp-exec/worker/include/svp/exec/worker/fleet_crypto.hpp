#pragma once

// The public-key operations of fleet join (fleet_join.hpp), on NIST P-256
// through the system Security framework (no extra dependency):
//
//   signing    ECDSA with SHA-256 (X9.62 DER signatures): coordinators sign
//              a join transcript with the fleet signing key; workers verify
//              it with the fleet public key from their join token.
//   agreement  ECDH (the shared x-coordinate): each join derives its
//              pairing secret from fresh keys of both sides, so nothing that
//              crosses the network, even to a holder of the join token,
//              reveals it.
//
// Keys travel as the Security framework's external representations: a
// public key is the X9.63 uncompressed point (04 || X || Y, 65 bytes), a
// private key that point followed by the scalar (97 bytes).

#include <cstddef>
#include <span>
#include <vector>

namespace svp::exec::worker {

inline constexpr std::size_t kEcPublicKeyBytes = 65;
inline constexpr std::size_t kEcPrivateKeyBytes = 97;
// P-256 ECDH yields the 32-byte x-coordinate.
inline constexpr std::size_t kEcSharedSecretBytes = 32;

struct EcKeyPair {
  std::vector<std::byte> private_key;
  std::vector<std::byte> public_key;
};

// A fresh key pair from the system CSPRNG. Throws WorkerError(io).
[[nodiscard]] EcKeyPair generate_ec_key_pair();

// The public half of a private key. Throws WorkerError(configuration) for a
// malformed key.
[[nodiscard]] std::vector<std::byte> ec_public_key_of(std::span<const std::byte> private_key);

// Throws WorkerError(configuration) for a malformed key, WorkerError(io) when
// signing fails.
[[nodiscard]] std::vector<std::byte> ec_sign(std::span<const std::byte> private_key,
                                             std::span<const std::byte> message);

// False for a malformed key or signature, or a signature that does not verify.
[[nodiscard]] bool ec_verify(std::span<const std::byte> public_key,
                             std::span<const std::byte> message,
                             std::span<const std::byte> signature) noexcept;

// ECDH between our private key and the peer's public key. Throws
// WorkerError(verification) for a malformed peer key.
[[nodiscard]] std::vector<std::byte> ec_shared_secret(std::span<const std::byte> private_key,
                                                      std::span<const std::byte> peer_public_key);

// `count` bytes from the system CSPRNG. Throws WorkerError(io).
[[nodiscard]] std::vector<std::byte> secure_random_bytes(std::size_t count);

}  // namespace svp::exec::worker
