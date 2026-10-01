#pragma once

#include "nw_ref.hpp"
#include "svp/exec/remote/remote_stream.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace svp::exec::remote::detail {

// How far the stream reads ahead of its consumer. One receive is kept
// outstanding while fewer than this many bytes are buffered, so the
// connection keeps draining while the frame decoder hashes payloads, and the
// consumer is woken once per delivery rather than once per TLS record. 4 MiB
// is several TCP windows at the Thunderbolt throughput measured in plan §3.4
// while adding little to the frame decoder's own per-frame bound
// (frame_limits.hpp).
inline constexpr std::size_t kReceiveAheadBytes = 4U * 1024U * 1024U;

enum class StreamPhase { connecting, ready, failed, cancelled };

// A Network framework connection presented as a blocking RemoteStream. The
// connection's callbacks run on the stream's own serial queue; blocking
// calls (read_some, write_all, wait_ready) run on the caller's thread.
class NwStream final : public RemoteStream {
 public:
  // Takes a connection that has not been started. `on_change`, when set, is
  // called on the connection queue after every phase change, with no lock
  // held; it must not block.
  NwStream(NwRef<nw_connection_t> connection, std::function<void()> on_change = {});
  ~NwStream() override;
  NwStream(const NwStream&) = delete;
  NwStream& operator=(const NwStream&) = delete;

  void start();

  [[nodiscard]] StreamPhase phase() const;
  // Start until ready; zero unless ready.
  [[nodiscard]] std::chrono::microseconds handshake_time() const;
  // Why the connection failed; empty unless failed.
  [[nodiscard]] std::string failure() const;
  // True when the failure came from TLS (for example a PSK mismatch).
  [[nodiscard]] bool failed_in_tls() const;

  // Blocks until the connection is ready, has failed, or `deadline` passes.
  // Returns true when ready.
  bool wait_ready(std::chrono::steady_clock::time_point deadline);

  [[nodiscard]] std::size_t read_some(std::span<std::byte> buffer) override;
  void write_all(std::span<const std::byte> bytes) override;
  void cancel() override;
  [[nodiscard]] TlsSession tls_session() const override;
  [[nodiscard]] std::string peer_description() const override;

  struct Shared;

 private:
  SerialQueue queue_;
  NwRef<nw_connection_t> connection_;
  std::shared_ptr<Shared> shared_;
};

}  // namespace svp::exec::remote::detail
