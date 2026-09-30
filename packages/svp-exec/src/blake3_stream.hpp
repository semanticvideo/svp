#pragma once

#include "svp/exec/blake3_digest.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace svp::exec::detail {

// Incremental BLAKE3 for streamed files, where blake3_digest() would need the
// whole content in memory.
class Blake3Stream {
 public:
  Blake3Stream();
  Blake3Stream(Blake3Stream&&) noexcept;
  Blake3Stream& operator=(Blake3Stream&&) noexcept;
  ~Blake3Stream();

  void update(std::span<const std::byte> bytes);
  [[nodiscard]] Blake3Digest finalize() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace svp::exec::detail
