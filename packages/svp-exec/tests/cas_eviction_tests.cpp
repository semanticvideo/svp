#include "storage_test_support.hpp"
#include "svp/exec/cas_store.hpp"

#include <fcntl.h>
#include <memory>
#include <sys/file.h>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-cas-eviction-tests";

// Every blob in these tests has this size, so limits are whole blob counts.
constexpr std::size_t kBlobBytes = 100;

std::string blob_content(char fill) {
  return std::string(kBlobBytes, fill);
}

// Deterministic recency: each call advances one second, so the order of
// put()/get() calls is exactly the LRU order.
struct SteppingClock {
  std::shared_ptr<std::filesystem::file_time_type> current =
      std::make_shared<std::filesystem::file_time_type>(
          std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));

  CacheClock clock() const {
    return [current = current] {
      *current += std::chrono::seconds(1);
      return *current;
    };
  }
};

CasStore store_with_limit(const fs::path& root, std::size_t blob_limit,
                          const SteppingClock& clock) {
  CasStoreOptions options;
  options.policy.max_bytes = blob_limit * kBlobBytes;
  options.now = clock.clock();
  return expect_ok(CasStore::at(root, options), "open store");
}

Blake3Digest put_blob(CasStore& store, char fill) {
  const std::string content = blob_content(fill);
  return expect_ok(store.put(as_byte_span(content)), "put blob");
}

void test_policy_defaults() {
  expect(CachePolicy{}.max_bytes == 50ULL * 1024 * 1024 * 1024,
         "RC2 §20.5.2 default maximum_size is 50 GiB");
}

void test_evicts_least_recently_used() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 2, clock);
  const Blake3Digest a = put_blob(store, 'a');
  const Blake3Digest b = put_blob(store, 'b');
  const Blake3Digest c = put_blob(store, 'c');
  const Blake3Digest d = put_blob(store, 'd');
  // A verified read makes `a` the most recently used.
  expect_ok(store.get(a), "touch a");

  const EvictionReport report = expect_ok(store.evict(), "evict");
  expect(report.evicted_blobs == 2 && report.evicted_bytes == 2 * kBlobBytes,
         "evicts down to the limit");
  expect(report.remaining_bytes == 2 * kBlobBytes, "remaining bytes at the limit");
  expect(!store.has(b) && !store.has(c), "least recently used blobs go first");
  expect(store.has(a) && store.has(d), "recently used blobs stay");

  const EvictionReport idle = expect_ok(store.evict(), "evict under limit");
  expect(idle.evicted_blobs == 0, "nothing to evict under the limit");
}

void test_repeated_put_refreshes_recency() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 1, clock);
  const Blake3Digest a = put_blob(store, 'a');
  const Blake3Digest b = put_blob(store, 'b');
  put_blob(store, 'a');  // idempotent put is a use
  expect_ok(store.evict(), "evict");
  expect(store.has(a) && !store.has(b), "re-put blob counts as recently used");
}

void test_pins_are_respected() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 1, clock);
  const Blake3Digest a = put_blob(store, 'a');
  const Blake3Digest b = put_blob(store, 'b');
  const Blake3Digest c = put_blob(store, 'c');
  {
    CasPinSet pins = expect_ok(store.pin_set("bs_0001"), "pin set");
    expect(pins.add(a).ok(), "pin a");
    expect(pins.add(b).ok(), "pin b");
    expect(pins.digests().size() == 2, "pin set tracks its digests");

    const EvictionReport report = expect_ok(store.evict(), "evict with pins");
    expect(store.has(a) && store.has(b), "pinned blobs survive eviction");
    expect(!store.has(c), "unpinned blob evicted");
    expect(report.pinned_blobs_kept == 2, "pinned candidates are reported");
    expect(report.remaining_bytes == 2 * kBlobBytes,
           "pinned blobs may keep usage above the limit");
  }
  // The holder released its pins.
  expect_ok(store.evict(), "evict after release");
  expect(!store.has(a) && store.has(b), "released pins are evictable in LRU order");
  expect(count_entries(store.root() / "pins") == 0, "released holder files are removed");
}

void test_pin_before_put_protects_blob() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 0, clock);
  CasPinSet pins = expect_ok(store.pin_set("ws_0001"), "pin set");
  const std::string content = blob_content('p');
  expect_cache_error(pins.add(blake3_digest(content)), CacheErrorCode::not_found,
                     "pinning an absent blob reports not_found");
  const Blake3Digest digest = put_blob(store, 'p');
  expect_ok(store.evict(), "evict");
  expect(store.has(digest), "a pin taken before put() still protects the blob");
}

void test_dead_holder_pins_expire() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 0, clock);
  const Blake3Digest a = put_blob(store, 'a');
  // A crashed holder leaves its files but not its lock.
  write_file(store.root() / "pins" / "bs_dead.lock", "");
  write_file(store.root() / "pins" / "bs_dead.pins", blake3_hex(a) + "\n");
  write_file(store.root() / "pins" / "bs_orphan.pins", blake3_hex(a) + "\n");

  expect_ok(store.evict(), "evict");
  expect(!store.has(a), "pins of a dead holder do not protect blobs");
  expect(count_entries(store.root() / "pins") == 0, "dead holder files are cleaned up");
}

void test_pin_holder_ids() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 1, clock);
  CasPinSet first = expect_ok(store.pin_set("bs_0001"), "first holder");
  expect_cache_error(store.pin_set("bs_0001"), CacheErrorCode::busy, "live holder id reused");
  expect_cache_error(store.pin_set("../escape"), CacheErrorCode::invalid_argument,
                     "holder id with a path separator");
}

void test_eviction_lock_busy() {
  TemporaryDirectory scratch(kSuite);
  SteppingClock clock;
  CasStore store = store_with_limit(scratch.path / "cache", 0, clock);
  const Blake3Digest a = put_blob(store, 'a');
  const fs::path lock_path = store.root() / "locks" / "eviction.lock";
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
  expect(fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0, "hold the eviction lock");
  expect_cache_error(store.evict(), CacheErrorCode::busy, "evict while another evicts");
  expect(store.has(a), "nothing removed while the lock is held");
  ::close(fd);
  expect_ok(store.evict(), "evict after the lock is released");
  expect(!store.has(a), "eviction proceeds once the lock is free");
}

void test_stale_pending_removed() {
  TemporaryDirectory scratch(kSuite);
  CasStore store = expect_ok(CasStore::at(scratch.path / "cache"), "open store");
  const fs::path stale = store.root() / "pending" / "1-1-1.pending";
  const fs::path fresh = store.root() / "pending" / "2-2-2.pending";
  write_file(stale, "abandoned");
  write_file(fresh, "in flight");
  fs::last_write_time(stale, fs::file_time_type::clock::now() - store.policy().stale_pending_age -
                                 std::chrono::minutes(1));

  const EvictionReport report = expect_ok(store.evict(), "evict");
  expect(report.stale_pending_removed == 1, "one stale pending file removed");
  expect(!fs::exists(stale), "stale pending file removed");
  expect(fs::exists(fresh), "a live writer's pending file is kept");
}

}  // namespace

int main() {
  return run_tests(kSuite, {
                               {"policy_defaults", test_policy_defaults},
                               {"evicts_least_recently_used", test_evicts_least_recently_used},
                               {"repeated_put_refreshes_recency",
                                test_repeated_put_refreshes_recency},
                               {"pins_are_respected", test_pins_are_respected},
                               {"pin_before_put_protects_blob", test_pin_before_put_protects_blob},
                               {"dead_holder_pins_expire", test_dead_holder_pins_expire},
                               {"pin_holder_ids", test_pin_holder_ids},
                               {"eviction_lock_busy", test_eviction_lock_busy},
                               {"stale_pending_removed", test_stale_pending_removed},
                           });
}
