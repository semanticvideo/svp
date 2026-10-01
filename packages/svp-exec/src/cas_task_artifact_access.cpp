#include "svp/exec/cas_task_artifact_access.hpp"

#include <sys/stat.h>

#include "cas_layout.hpp"
#include "svp/exec/exec_error.hpp"

#include <filesystem>
#include <utility>

namespace svp::exec {
namespace {

namespace fs = std::filesystem;

std::string describe(const ArtifactRef& ref) {
  return ref.role + " " + blake3_hex(ref.blake3);
}

[[noreturn]] void unavailable(std::string_view what, const ArtifactRef& ref,
                              std::string_view reason) {
  throw ExecError(ExecErrorCode::unresolved_input,
                  std::string(what) + " " + describe(ref) + " is not available: " +
                      std::string(reason));
}

}  // namespace

CasTaskArtifactAccess::CasTaskArtifactAccess(CasStore store, const std::string& pin_holder_id)
    : store_(std::move(store)) {
  CacheResult<CasPinSet> pins = store_.pin_set(pin_holder_id);
  if (pins) {
    pins_.emplace(std::move(pins).value());
  } else {
    warnings_.push_back(pins.error());
  }
}

void CasTaskArtifactAccess::pin(const Blake3Digest& digest) {
  const std::lock_guard lock(mutex_);
  if (!pins_) {
    return;
  }
  const CacheStatus status = pins_->add(digest);
  // not_found only says the blob is not stored yet; the pin is kept and
  // protects the put() that follows.
  if (!status && status.error().code != CacheErrorCode::not_found) {
    warnings_.push_back(status.error());
  }
}

std::optional<CasTaskArtifactAccess::FileIdentity> CasTaskArtifactAccess::file_identity(
    const std::filesystem::path& path) {
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) {
    return std::nullopt;
  }
#if defined(__APPLE__)
  const struct timespec& modified = status.st_mtimespec;
#else
  const struct timespec& modified = status.st_mtim;
#endif
  return FileIdentity{.device = static_cast<std::uint64_t>(status.st_dev),
                      .inode = static_cast<std::uint64_t>(status.st_ino),
                      .size = static_cast<std::uint64_t>(status.st_size),
                      .mtime_ns = static_cast<std::int64_t>(modified.tv_sec) * 1'000'000'000 +
                                  modified.tv_nsec};
}

ResolvedInputs CasTaskArtifactAccess::resolve_inputs(const TaskSpec& spec) {
  ResolvedInputs resolved;
  for (const auto& [name, ref] : spec.inputs) {
    pin(ref.blake3);
    const std::optional<FileIdentity> before =
        file_identity(detail::cas_blob_path(store_.root(), ref.blake3));
    bool known_good = false;
    {
      const std::lock_guard lock(mutex_);
      const auto found = verified_inputs_.find(ref.blake3);
      known_good = before && found != verified_inputs_.end() && found->second == *before;
    }
    if (!known_good) {
      const CacheStatus verified = store_.verify(ref.blake3);
      if (!verified) {
        unavailable("input `" + name + "`", ref, verified.error().message);
      }
      const std::lock_guard lock(mutex_);
      if (pins_ && before) {
        verified_inputs_.insert_or_assign(ref.blake3, *before);
      }
    }
    fs::path path = detail::cas_blob_path(store_.root(), ref.blake3);
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size != ref.bytes) {
      unavailable("input `" + name + "`", ref,
                  error ? error.message() : "stored length differs from the reference");
    }
    resolved.emplace(name, ResolvedInput{.ref = ref, .path = std::move(path)});
  }
  return resolved;
}

std::vector<FramePayload> CasTaskArtifactAccess::read_outputs(const TaskResult& result) {
  std::vector<FramePayload> payloads;
  payloads.reserve(result.outputs.size());
  for (const ArtifactRef& ref : result.outputs) {
    {
      const std::lock_guard lock(mutex_);
      if (const auto held = memory_fallback_.find(ref.blake3); held != memory_fallback_.end()) {
        payloads.push_back(held->second);
        continue;
      }
    }
    CacheResult<std::vector<std::byte>> bytes = store_.get(ref.blake3);
    if (!bytes) {
      unavailable("output", ref, bytes.error().message);
    }
    if (bytes.value().size() != ref.bytes) {
      unavailable("output", ref, "stored length differs from the reference");
    }
    payloads.push_back(std::move(bytes).value());
  }
  return payloads;
}

ArtifactRef CasTaskArtifactAccess::put(std::span<const std::byte> bytes, std::string media_type,
                                       std::string role) {
  ArtifactRef ref = make_artifact_ref(bytes, std::move(media_type), std::move(role));
  validate_artifact_ref(ref);
  pin(ref.blake3);
  const CacheResult<Blake3Digest> stored = store_.put(bytes);
  if (!stored) {
    const std::lock_guard lock(mutex_);
    warnings_.push_back(stored.error());
    if (memory_fallback_.emplace(ref.blake3, std::vector<std::byte>(bytes.begin(), bytes.end()))
            .second) {
      memory_fallback_bytes_ += bytes.size();
    }
  }
  return ref;
}

bool CasTaskArtifactAccess::pinned() const {
  const std::lock_guard lock(mutex_);
  return pins_.has_value();
}

std::vector<Blake3Digest> CasTaskArtifactAccess::pinned_digests() const {
  const std::lock_guard lock(mutex_);
  return pins_ ? pins_->digests() : std::vector<Blake3Digest>{};
}

std::vector<CacheError> CasTaskArtifactAccess::cache_warnings() const {
  const std::lock_guard lock(mutex_);
  return warnings_;
}

std::uint64_t CasTaskArtifactAccess::memory_fallback_bytes() const {
  const std::lock_guard lock(mutex_);
  return memory_fallback_bytes_;
}

}  // namespace svp::exec
