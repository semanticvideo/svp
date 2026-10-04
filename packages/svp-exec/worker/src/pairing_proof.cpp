#include "svp/exec/worker/pairing_proof.hpp"

#include "svp/exec/blake3_digest.hpp"

namespace svp::exec::worker {

PairingProof make_pairing_proof(const svp::exec::remote::PairingKey& key,
                                const std::vector<std::byte>& exporter) {
  const Blake3Digest proof_key = blake3_derive_key(kPairingProofKeyContext, key.secret);
  return PairingProof{.pairing_id = key.pairing_id,
                      .proof = blake3_hex(blake3_keyed_hash(proof_key, exporter))};
}

std::optional<PairingProof> prove_pairing(const svp::exec::remote::RemoteStream& stream,
                                          const svp::exec::remote::PairingKey& key) {
  const std::optional<std::vector<std::byte>> exporter =
      stream.export_keying_material(kPairingProofExporterLabel, kPairingProofExporterBytes);
  if (!exporter) {
    return std::nullopt;
  }
  return make_pairing_proof(key, *exporter);
}

bool verify_pairing_proof(const PairingProof& proof, const std::vector<std::byte>& exporter,
                          const svp::exec::remote::PairingKey& key) {
  if (proof.pairing_id != key.pairing_id || exporter.size() != kPairingProofExporterBytes) {
    return false;
  }
  const std::string expected = make_pairing_proof(key, exporter).proof;
  // Constant-time comparison: the proof is a MAC.
  if (expected.size() != proof.proof.size()) {
    return false;
  }
  unsigned difference = 0;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    difference |= static_cast<unsigned>(expected[index] ^ proof.proof[index]);
  }
  return difference == 0;
}

void ProvingFrameWriter::write(const Frame& frame) {
  if (frame.type != MessageType::hello || !proof_ || !frame.body.is_object()) {
    inner_.write(frame);
    return;
  }
  Frame proved = frame;
  proved.body["pairing"] = pairing_proof_to_json(*proof_);
  inner_.write(proved);
}

std::optional<Frame> AckObservingFrameReader::read() {
  std::optional<Frame> frame = inner_.read();
  if (frame && frame->type == MessageType::hello_ack && on_ack_) {
    try {
      on_ack_(hello_ack_from_frame(*frame));
    } catch (const std::exception&) {
      // The session's own reader reports a malformed HELLO_ACK.
    }
  }
  return frame;
}

}  // namespace svp::exec::worker
