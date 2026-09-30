#include "svp/exec/cas_pin.hpp"

#include "cache_error_mapping.hpp"
#include "cas_layout.hpp"
#include "cas_pin_registry.hpp"
#include "durable_io.hpp"
#include "verified_blob.hpp"

#include <algorithm>
#include <fstream>

namespace svp::exec {
namespace detail {
namespace {

namespace fs = std::filesystem;

fs::path pin_file(const fs::path& root, const std::string& holder_id, std::string_view suffix) {
  return cas_paths(root).pins / (holder_id + std::string(suffix));
}

std::error_code write_pin_list(const CasPinSetState& state) {
  std::string text;
  for (const Blake3Digest& digest : state.digests) {
    text += blake3_hex(digest);
    text += '\n';
  }
  const fs::path staged = cas_paths(state.root).pending / pending_file_name();
  if (const auto error = write_new_file_synced(
          staged, std::as_bytes(std::span(text.data(), text.size())))) {
    std::error_code ignored;
    fs::remove(staged, ignored);
    return error;
  }
  std::error_code error;
  fs::rename(staged, pin_file(state.root, state.holder_id, kPinListSuffix), error);
  if (error) {
    std::error_code ignored;
    fs::remove(staged, ignored);
  }
  return error;
}

void read_pin_list(const fs::path& path, std::set<Blake3Digest>& pinned) {
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (const auto digest = parse_blake3_hex(line)) {
      pinned.insert(*digest);
    }
  }
}

}  // namespace

CasPinSetState::~CasPinSetState() {
  if (!holder_lock.held()) {
    return;
  }
  std::error_code ignored;
  fs::remove(pin_file(root, holder_id, kPinListSuffix), ignored);
  fs::remove(pin_file(root, holder_id, kPinLockSuffix), ignored);
  holder_lock.release();
}

std::error_code read_live_pins(const fs::path& root, std::set<Blake3Digest>& pinned) {
  const fs::path pins_dir = cas_paths(root).pins;
  std::error_code error;
  std::vector<fs::path> lock_files;
  std::vector<fs::path> list_files;
  for (fs::directory_iterator entry(pins_dir, error), end; !error && entry != end;
       entry.increment(error)) {
    const fs::path& path = entry->path();
    if (path.extension() == kPinLockSuffix) {
      lock_files.push_back(path);
    } else if (path.extension() == kPinListSuffix) {
      list_files.push_back(path);
    }
  }
  if (error) {
    return error;
  }
  for (const fs::path& lock_path : lock_files) {
    LockAttempt attempt = lock_file(lock_path, LockMode::exclusive, LockWait::try_once);
    fs::path list_path = lock_path;
    list_path.replace_extension(kPinListSuffix);
    if (attempt.outcome == LockOutcome::contended) {
      read_pin_list(list_path, pinned);
      continue;
    }
    if (attempt.outcome == LockOutcome::failed) {
      // Liveness unknown: refusing to evict is the only safe answer.
      return attempt.error;
    }
    std::error_code ignored;
    fs::remove(list_path, ignored);
    fs::remove(lock_path, ignored);
  }
  // A list without a lock file belongs to a holder that finished tearing down
  // or crashed before creating its lock; it pins nothing.
  for (const fs::path& list_path : list_files) {
    fs::path lock_path = list_path;
    lock_path.replace_extension(kPinLockSuffix);
    std::error_code exists_error;
    if (!fs::exists(lock_path, exists_error) && !exists_error) {
      std::error_code ignored;
      fs::remove(list_path, ignored);
    }
  }
  return {};
}

}  // namespace detail

CasPinSet::CasPinSet(std::unique_ptr<detail::CasPinSetState> state) : state_(std::move(state)) {}
CasPinSet::CasPinSet(CasPinSet&&) noexcept = default;
CasPinSet& CasPinSet::operator=(CasPinSet&&) noexcept = default;
CasPinSet::~CasPinSet() = default;

const std::string& CasPinSet::holder_id() const noexcept {
  return state_->holder_id;
}

const std::vector<Blake3Digest>& CasPinSet::digests() const noexcept {
  return state_->digests;
}

CacheStatus CasPinSet::add(const Blake3Digest& digest) {
  const detail::CasPaths paths = detail::cas_paths(state_->root);
  detail::LockAttempt eviction =
      detail::lock_file(paths.eviction_lock, detail::LockMode::shared, detail::LockWait::block);
  if (eviction.outcome != detail::LockOutcome::acquired) {
    return detail::cache_error(eviction.error, "cache pin: eviction lock");
  }
  const bool already_pinned =
      std::find(state_->digests.begin(), state_->digests.end(), digest) != state_->digests.end();
  if (!already_pinned) {
    state_->digests.push_back(digest);
    if (const auto error = detail::write_pin_list(*state_)) {
      state_->digests.pop_back();
      return detail::cache_error(error, "cache pin: write pin list");
    }
  }
  std::error_code exists_error;
  if (!std::filesystem::exists(detail::cas_blob_path(state_->root, digest), exists_error)) {
    return detail::cache_error(CacheErrorCode::not_found,
                               "cache pin: blob " + blake3_hex(digest) + " is not stored");
  }
  return CacheOk{};
}

}  // namespace svp::exec
