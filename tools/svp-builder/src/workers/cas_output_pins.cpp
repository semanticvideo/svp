#include "cas_output_pins.hpp"

#include <stdexcept>
#include <utility>

namespace svp::builder::workers {

CasOutputPins::CasOutputPins(std::filesystem::path cas_root, std::string holder)
    : cas_root_(std::move(cas_root)), holder_(std::move(holder)) {}

svp::exec::Blake3Digest CasOutputPins::store(const std::filesystem::path& file) {
  const std::lock_guard lock(mutex_);
  if (!store_) {
    svp::exec::CacheResult<svp::exec::CasStore> opened = svp::exec::CasStore::at(cas_root_);
    if (!opened) {
      throw std::runtime_error("cannot open the worker cache: " + opened.error().message);
    }
    store_.emplace(std::move(opened).value());
    svp::exec::CacheResult<svp::exec::CasPinSet> pins = store_->pin_set(holder_);
    if (!pins) {
      throw std::runtime_error("cannot pin in the worker cache: " + pins.error().message);
    }
    pins_.emplace(std::move(pins).value());
  }
  svp::exec::CacheResult<svp::exec::Blake3Digest> stored = store_->put_file(file);
  if (!stored) {
    throw std::runtime_error("cannot store " + file.filename().string() +
                             " in the worker cache: " + stored.error().message);
  }
  const svp::exec::CacheStatus pinned = pins_->add(stored.value());
  if (!pinned) {
    throw std::runtime_error("cannot pin " + file.filename().string() +
                             " in the worker cache: " + pinned.error().message);
  }
  return stored.value();
}

}  // namespace svp::builder::workers
