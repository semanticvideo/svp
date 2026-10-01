#pragma once

// RAII ownership of Network framework and dispatch objects from C++ (no ARC:
// every NW_RETURNS_RETAINED result must be released exactly once).

#include <Network/Network.h>
#include <dispatch/dispatch.h>

#include <utility>

namespace svp::exec::remote::detail {

template <typename T>
class NwRef {
 public:
  NwRef() = default;
  // Takes over one reference the caller already owns (a create/copy result).
  [[nodiscard]] static NwRef adopt(T object) {
    NwRef ref;
    ref.object_ = object;
    return ref;
  }
  // Adds a reference to a borrowed object (a block argument).
  [[nodiscard]] static NwRef retain(T object) {
    if (object != nullptr) {
      nw_retain(object);
    }
    return adopt(object);
  }

  NwRef(const NwRef& other) : object_(other.object_) {
    if (object_ != nullptr) {
      nw_retain(object_);
    }
  }
  NwRef& operator=(const NwRef& other) {
    NwRef copy(other);
    std::swap(object_, copy.object_);
    return *this;
  }
  NwRef(NwRef&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {}
  NwRef& operator=(NwRef&& other) noexcept {
    if (this != &other) {
      reset();
      object_ = std::exchange(other.object_, nullptr);
    }
    return *this;
  }
  ~NwRef() { reset(); }

  void reset() {
    if (object_ != nullptr) {
      nw_release(object_);
      object_ = nullptr;
    }
  }
  [[nodiscard]] T get() const { return object_; }
  explicit operator bool() const { return object_ != nullptr; }

 private:
  T object_ = nullptr;
};

// A private serial queue for one connection, listener, or browser.
class SerialQueue {
 public:
  explicit SerialQueue(const char* label)
      : queue_(dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL)) {}
  ~SerialQueue() { dispatch_release(queue_); }
  SerialQueue(const SerialQueue&) = delete;
  SerialQueue& operator=(const SerialQueue&) = delete;
  [[nodiscard]] dispatch_queue_t get() const { return queue_; }

 private:
  dispatch_queue_t queue_;
};

}  // namespace svp::exec::remote::detail
