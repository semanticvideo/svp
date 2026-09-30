#include "blake3_stream.hpp"

#include <blake3.h>

namespace svp::exec::detail {

struct Blake3Stream::State {
  blake3_hasher hasher;
};

Blake3Stream::Blake3Stream() : state_(std::make_unique<State>()) {
  blake3_hasher_init(&state_->hasher);
}

Blake3Stream::Blake3Stream(Blake3Stream&&) noexcept = default;
Blake3Stream& Blake3Stream::operator=(Blake3Stream&&) noexcept = default;
Blake3Stream::~Blake3Stream() = default;

void Blake3Stream::update(std::span<const std::byte> bytes) {
  blake3_hasher_update(&state_->hasher, bytes.data(), bytes.size());
}

Blake3Digest Blake3Stream::finalize() const {
  Blake3Digest digest{};
  blake3_hasher_finalize(&state_->hasher, digest.data(), digest.size());
  return digest;
}

}  // namespace svp::exec::detail
