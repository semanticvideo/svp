#include "tls_psk_parameters.hpp"

#include "svp/exec/remote/remote_error.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecProtocolMetadata.h>
#include <Security/SecProtocolOptions.h>

#include <cstdlib>
#include <vector>

namespace svp::exec::remote::detail {
namespace {

std::string cf_string(CFStringRef text) {
  if (text == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(text);
  const CFIndex capacity =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::vector<char> buffer(static_cast<std::size_t>(capacity));
  if (!CFStringGetCString(text, buffer.data(), capacity, kCFStringEncodingUTF8)) {
    return {};
  }
  return std::string(buffer.data());
}

std::string_view domain_name(nw_error_domain_t domain) {
  switch (domain) {
    case nw_error_domain_posix:
      return "posix";
    case nw_error_domain_dns:
      return "dns";
    case nw_error_domain_tls:
      return "tls";
    default:
      return "network";
  }
}

}  // namespace

NwRef<nw_parameters_t> make_tls_psk_parameters(
    const PairingKey& key, const TransportPolicy& policy,
    const std::vector<std::string>& application_protocols) {
  return make_tls_psk_parameters(std::vector<PairingKey>{key}, policy, application_protocols);
}

NwRef<nw_parameters_t> make_tls_psk_parameters(
    const std::vector<PairingKey>& keys, const TransportPolicy& policy,
    const std::vector<std::string>& application_protocols) {
  if (keys.empty()) {
    throw RemoteTransportError(RemoteErrorCode::invalid_configuration,
                               "a TLS-PSK endpoint needs at least one key");
  }
  for (const PairingKey& key : keys) {
    validate_pairing_key(key);
  }
  validate_transport_policy(policy);
  // Captured by value: the blocks own copies whenever the framework runs them.
  const std::vector<PairingKey> psks = keys;
  const std::vector<std::string> protocols = application_protocols;
  const int keepalive_idle = static_cast<int>(policy.keepalive_idle.count());
  const int keepalive_interval = static_cast<int>(policy.keepalive_interval.count());
  const std::uint32_t keepalive_probes = policy.keepalive_probes;

  nw_parameters_t parameters = nw_parameters_create_secure_tcp(
      ^(nw_protocol_options_t tls_options) {
        sec_protocol_options_t options = nw_tls_copy_sec_protocol_options(tls_options);
        for (const PairingKey& key : psks) {
          dispatch_data_t psk = dispatch_data_create(key.secret.data(), key.secret.size(),
                                                     nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
          dispatch_data_t identity =
              dispatch_data_create(key.pairing_id.data(), key.pairing_id.size(), nullptr,
                                   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
          sec_protocol_options_add_pre_shared_key(options, psk, identity);
          dispatch_release(psk);
          dispatch_release(identity);
        }
        sec_protocol_options_append_tls_ciphersuite(
            options, static_cast<tls_ciphersuite_t>(kTlsPskWithAes128GcmSha256));
        sec_protocol_options_set_min_tls_protocol_version(options, tls_protocol_version_TLSv12);
        sec_protocol_options_set_max_tls_protocol_version(options, tls_protocol_version_TLSv12);
        for (const std::string& protocol : protocols) {
          sec_protocol_options_add_tls_application_protocol(options, protocol.c_str());
        }
        // Every connection proves the pairing secret afresh.
        sec_protocol_options_set_tls_resumption_enabled(options, false);
        sec_protocol_options_set_tls_tickets_enabled(options, false);
        sec_release(options);
      },
      ^(nw_protocol_options_t tcp_options) {
        nw_tcp_options_set_no_delay(tcp_options, true);
        nw_tcp_options_set_enable_keepalive(tcp_options, true);
        nw_tcp_options_set_keepalive_idle_time(tcp_options, keepalive_idle);
        nw_tcp_options_set_keepalive_interval(tcp_options, keepalive_interval);
        nw_tcp_options_set_keepalive_count(tcp_options, keepalive_probes);
      });
  nw_parameters_set_include_peer_to_peer(parameters, false);
  return NwRef<nw_parameters_t>::adopt(parameters);
}

TlsSession read_tls_session(nw_connection_t connection) {
  TlsSession session;
  nw_protocol_definition_t definition = nw_protocol_copy_tls_definition();
  nw_protocol_metadata_t metadata = nw_connection_copy_protocol_metadata(connection, definition);
  nw_release(definition);
  if (metadata == nullptr) {
    return session;
  }
  sec_protocol_metadata_t security = nw_tls_copy_sec_protocol_metadata(metadata);
  if (security != nullptr) {
    session.protocol_version = static_cast<std::uint16_t>(
        sec_protocol_metadata_get_negotiated_tls_protocol_version(security));
    session.cipher_suite = static_cast<std::uint16_t>(
        sec_protocol_metadata_get_negotiated_tls_ciphersuite(security));
    if (const char* protocol = sec_protocol_metadata_copy_negotiated_protocol(security)) {
      session.application_protocol = protocol;
      std::free(const_cast<char*>(protocol));
    }
    sec_release(security);
  }
  nw_release(metadata);
  return session;
}

bool is_expected_tls_session(const TlsSession& session) {
  return session.protocol_version == kTls12ProtocolVersion &&
         session.cipher_suite == kTlsPskWithAes128GcmSha256;
}

std::string describe_nw_error(nw_error_t error) {
  if (error == nullptr) {
    return "no error";
  }
  std::string text = std::string(domain_name(nw_error_get_error_domain(error))) + " error " +
                     std::to_string(nw_error_get_error_code(error));
  if (CFErrorRef cf_error = nw_error_copy_cf_error(error)) {
    CFStringRef description = CFErrorCopyDescription(cf_error);
    const std::string detail = cf_string(description);
    if (description != nullptr) {
      CFRelease(description);
    }
    CFRelease(cf_error);
    if (!detail.empty()) {
      text += ": " + detail;
    }
  }
  return text;
}

}  // namespace svp::exec::remote::detail
