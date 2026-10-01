#include "svp/exec/remote/pairing_key.hpp"

#include "svp/exec/remote/remote_error.hpp"

#include <algorithm>

namespace svp::exec::remote {
namespace {

bool is_id_character(char character) {
  return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '.' || character == '_' ||
         character == '-';
}

[[noreturn]] void invalid(std::string message) {
  throw RemoteTransportError(RemoteErrorCode::invalid_configuration, std::move(message));
}

}  // namespace

void validate_pairing_key(const PairingKey& key) {
  if (key.pairing_id.empty() || key.pairing_id.size() > kMaxPairingIdBytes ||
      !std::all_of(key.pairing_id.begin(), key.pairing_id.end(), is_id_character)) {
    invalid("pairing id must be 1-" + std::to_string(kMaxPairingIdBytes) +
            " characters from [A-Za-z0-9._-]");
  }
  if (key.secret.size() < kMinPairingSecretBytes) {
    invalid("pairing secret must be at least " + std::to_string(kMinPairingSecretBytes) +
            " bytes");
  }
}

}  // namespace svp::exec::remote
