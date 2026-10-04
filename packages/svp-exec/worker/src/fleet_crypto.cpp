#include "svp/exec/worker/fleet_crypto.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <sys/random.h>

#include <algorithm>
#include <string>

namespace svp::exec::worker {
namespace {

// The P-256 key size the Security framework is asked for.
constexpr int kEcKeySizeBits = 256;
// getentropy(2) returns at most this many bytes per call.
constexpr std::size_t kMaxEntropyChunk = 256;

// Owns one CoreFoundation reference.
template <typename T>
class CfRef {
 public:
  explicit CfRef(T value = nullptr) : value_(value) {}
  ~CfRef() {
    if (value_ != nullptr) {
      CFRelease(value_);
    }
  }
  CfRef(const CfRef&) = delete;
  CfRef& operator=(const CfRef&) = delete;
  CfRef(CfRef&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  CfRef& operator=(CfRef&&) = delete;
  [[nodiscard]] T get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

 private:
  T value_;
};

CfRef<CFDataRef> make_data(std::span<const std::byte> bytes) {
  return CfRef<CFDataRef>(CFDataCreate(kCFAllocatorDefault,
                                       reinterpret_cast<const UInt8*>(bytes.data()),
                                       static_cast<CFIndex>(bytes.size())));
}

std::vector<std::byte> bytes_of(CFDataRef data) {
  const auto* begin = reinterpret_cast<const std::byte*>(CFDataGetBytePtr(data));
  return std::vector<std::byte>(begin, begin + CFDataGetLength(data));
}

std::string describe(CFErrorRef error) {
  if (error == nullptr) {
    return "unknown error";
  }
  CfRef<CFStringRef> text(CFErrorCopyDescription(error));
  if (!text) {
    return "error " + std::to_string(CFErrorGetCode(error));
  }
  char buffer[512] = {};
  CFStringGetCString(text.get(), buffer, sizeof(buffer), kCFStringEncodingUTF8);
  return buffer;
}

// A SecKey from an external representation, or null.
CfRef<SecKeyRef> import_key(std::span<const std::byte> bytes, bool is_private) {
  const std::size_t expected = is_private ? kEcPrivateKeyBytes : kEcPublicKeyBytes;
  if (bytes.size() != expected) {
    return CfRef<SecKeyRef>();
  }
  const CfRef<CFDataRef> data = make_data(bytes);
  const int bits = kEcKeySizeBits;
  const CfRef<CFNumberRef> size(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits));
  const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits};
  const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom,
                          is_private ? kSecAttrKeyClassPrivate : kSecAttrKeyClassPublic,
                          size.get()};
  const CfRef<CFDictionaryRef> attributes(
      CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks));
  CFErrorRef error = nullptr;
  SecKeyRef key = SecKeyCreateWithData(data.get(), attributes.get(), &error);
  if (error != nullptr) {
    CFRelease(error);
  }
  return CfRef<SecKeyRef>(key);
}

CfRef<SecKeyRef> require_private(std::span<const std::byte> bytes) {
  CfRef<SecKeyRef> key = import_key(bytes, true);
  if (!key) {
    throw WorkerError(WorkerErrorCode::configuration, "malformed P-256 private key");
  }
  return key;
}

std::vector<std::byte> public_of(SecKeyRef private_key) {
  const CfRef<SecKeyRef> public_key(SecKeyCopyPublicKey(private_key));
  if (!public_key) {
    throw WorkerError(WorkerErrorCode::configuration, "the private key has no public key");
  }
  CFErrorRef error = nullptr;
  const CfRef<CFDataRef> data(SecKeyCopyExternalRepresentation(public_key.get(), &error));
  if (!data) {
    const std::string reason = describe(error);
    if (error != nullptr) CFRelease(error);
    throw WorkerError(WorkerErrorCode::io, "cannot export a public key: " + reason);
  }
  return bytes_of(data.get());
}

}  // namespace

std::vector<std::byte> secure_random_bytes(std::size_t count) {
  std::vector<std::byte> bytes(count);
  for (std::size_t offset = 0; offset < count; offset += kMaxEntropyChunk) {
    const std::size_t chunk = std::min(kMaxEntropyChunk, count - offset);
    if (::getentropy(bytes.data() + offset, chunk) != 0) {
      throw WorkerError(WorkerErrorCode::io, "the system has no entropy");
    }
  }
  return bytes;
}

EcKeyPair generate_ec_key_pair() {
  const int bits = kEcKeySizeBits;
  const CfRef<CFNumberRef> size(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits));
  // Not kSecAttrIsPermanent: the key never touches a keychain.
  const void* keys[] = {kSecAttrKeyType, kSecAttrKeySizeInBits};
  const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom, size.get()};
  const CfRef<CFDictionaryRef> attributes(
      CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks));
  CFErrorRef error = nullptr;
  const CfRef<SecKeyRef> key(SecKeyCreateRandomKey(attributes.get(), &error));
  if (!key) {
    const std::string reason = describe(error);
    if (error != nullptr) CFRelease(error);
    throw WorkerError(WorkerErrorCode::io, "cannot generate a P-256 key: " + reason);
  }
  const CfRef<CFDataRef> data(SecKeyCopyExternalRepresentation(key.get(), &error));
  if (!data) {
    const std::string reason = describe(error);
    if (error != nullptr) CFRelease(error);
    throw WorkerError(WorkerErrorCode::io, "cannot export a P-256 key: " + reason);
  }
  EcKeyPair pair;
  pair.private_key = bytes_of(data.get());
  pair.public_key = public_of(key.get());
  return pair;
}

std::vector<std::byte> ec_public_key_of(std::span<const std::byte> private_key) {
  return public_of(require_private(private_key).get());
}

std::vector<std::byte> ec_sign(std::span<const std::byte> private_key,
                               std::span<const std::byte> message) {
  const CfRef<SecKeyRef> key = require_private(private_key);
  const CfRef<CFDataRef> data = make_data(message);
  CFErrorRef error = nullptr;
  const CfRef<CFDataRef> signature(SecKeyCreateSignature(
      key.get(), kSecKeyAlgorithmECDSASignatureMessageX962SHA256, data.get(), &error));
  if (!signature) {
    const std::string reason = describe(error);
    if (error != nullptr) CFRelease(error);
    throw WorkerError(WorkerErrorCode::io, "cannot sign: " + reason);
  }
  return bytes_of(signature.get());
}

bool ec_verify(std::span<const std::byte> public_key, std::span<const std::byte> message,
               std::span<const std::byte> signature) noexcept {
  const CfRef<SecKeyRef> key = import_key(public_key, false);
  if (!key || signature.empty()) {
    return false;
  }
  const CfRef<CFDataRef> data = make_data(message);
  const CfRef<CFDataRef> signature_data = make_data(signature);
  CFErrorRef error = nullptr;
  const bool verified =
      SecKeyVerifySignature(key.get(), kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
                            data.get(), signature_data.get(), &error);
  if (error != nullptr) {
    CFRelease(error);
  }
  return verified;
}

std::vector<std::byte> ec_shared_secret(std::span<const std::byte> private_key,
                                        std::span<const std::byte> peer_public_key) {
  const CfRef<SecKeyRef> key = require_private(private_key);
  const CfRef<SecKeyRef> peer = import_key(peer_public_key, false);
  if (!peer) {
    throw WorkerError(WorkerErrorCode::verification, "malformed P-256 public key from the peer");
  }
  const CfRef<CFDictionaryRef> parameters(CFDictionaryCreate(
      kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks));
  CFErrorRef error = nullptr;
  const CfRef<CFDataRef> shared(SecKeyCopyKeyExchangeResult(
      key.get(), kSecKeyAlgorithmECDHKeyExchangeStandard, peer.get(), parameters.get(), &error));
  if (!shared) {
    const std::string reason = describe(error);
    if (error != nullptr) CFRelease(error);
    throw WorkerError(WorkerErrorCode::verification, "ECDH failed: " + reason);
  }
  std::vector<std::byte> secret = bytes_of(shared.get());
  if (secret.size() != kEcSharedSecretBytes) {
    throw WorkerError(WorkerErrorCode::verification, "ECDH returned an unexpected length");
  }
  return secret;
}

}  // namespace svp::exec::worker
