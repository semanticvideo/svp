#pragma once

// Which pairing a coordinator's session belongs to (hello_messages.hpp HELLO
// `pairing`). A worker serves every pairing on one TLS-PSK listener, and the
// TLS stack does not report which pre-shared key the peer used, so the
// coordinator proves it in-band, bound to the connection:
//
//   key   = BLAKE3 derive_key(kPairingProofKeyContext, pairing secret)
//   proof = BLAKE3 keyed_hash(key, TLS exporter(kPairingProofExporterLabel,
//                                                kPairingProofExporterBytes))
//
// The exporter (RFC 5705) is unique to the connection and derived from the
// PSK the handshake used, so a proof is neither forgeable without that
// pairing's secret nor replayable on another connection.
//
// The coordinator never builds the proof by hand: its connection's frame
// writer adds it to the HELLO it sends (ProvingFrameWriter), and a reader
// beside it learns the worker id the HELLO_ACK reports (WorkerIdLearningReader,
// worker_id_book.hpp), on every path that opens a worker session.

#include "svp/exec/frame_stream.hpp"
#include "svp/exec/remote/pairing_key.hpp"
#include "svp/exec/remote/remote_stream.hpp"
#include "svp/exec/worker/hello_messages.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace svp::exec::worker {

// Exporter labels must begin with "EXPORTER" (RFC 5705 §4).
inline constexpr std::string_view kPairingProofExporterLabel = "EXPORTER-svp-worker-pairing-v1";
// One BLAKE3 key's worth.
inline constexpr std::size_t kPairingProofExporterBytes = 32;
inline constexpr std::string_view kPairingProofKeyContext = "svp worker pairing proof key v1";

// The proof over `exporter` for `key`.
[[nodiscard]] PairingProof make_pairing_proof(const svp::exec::remote::PairingKey& key,
                                              const std::vector<std::byte>& exporter);

// The proof for `key` on `stream`; nullopt when the stream offers no exporter.
[[nodiscard]] std::optional<PairingProof> prove_pairing(
    const svp::exec::remote::RemoteStream& stream, const svp::exec::remote::PairingKey& key);

// True when `proof` names `key`'s pairing and matches `exporter`.
[[nodiscard]] bool verify_pairing_proof(const PairingProof& proof,
                                        const std::vector<std::byte>& exporter,
                                        const svp::exec::remote::PairingKey& key);

// Writes frames through `inner`, adding `proof` to every HELLO.
class ProvingFrameWriter final : public FrameWriter {
 public:
  ProvingFrameWriter(FrameWriter& inner, std::optional<PairingProof> proof)
      : inner_(inner), proof_(std::move(proof)) {}
  void write(const Frame& frame) override;

 private:
  FrameWriter& inner_;
  std::optional<PairingProof> proof_;
};

// Reads frames through `inner`, calling `on_ack` with every HELLO_ACK that
// decodes (the worker id it reports, for example).
class AckObservingFrameReader final : public FrameReader {
 public:
  AckObservingFrameReader(FrameReader& inner,
                          std::function<void(const WorkerHelloAck&)> on_ack)
      : inner_(inner), on_ack_(std::move(on_ack)) {}
  [[nodiscard]] std::optional<Frame> read() override;

 private:
  FrameReader& inner_;
  std::function<void(const WorkerHelloAck&)> on_ack_;
};

}  // namespace svp::exec::worker
