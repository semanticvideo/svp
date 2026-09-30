#include "storage_test_support.hpp"
#include "svp/exec/cache_root.hpp"
#include "svp/exec/cas_store.hpp"

#include <unistd.h>

namespace {

using namespace svp::exec;
using namespace svp::exec::test;

constexpr std::string_view kSuite = "svp-exec-cas-store-tests";

fs::path expected_blob_path(const fs::path& root, const Blake3Digest& digest) {
  const std::string hex = blake3_hex(digest);
  return root / "blobs" / "b3" / hex.substr(0, 2) / hex.substr(2);
}

void test_put_get_round_trip() {
  TemporaryDirectory scratch(kSuite);
  CasStore store = expect_ok(CasStore::at(scratch.path / "cache"), "open store");
  constexpr std::string_view kContent = "frame batch 0001 detections";

  const Blake3Digest digest = expect_ok(store.put(as_byte_span(kContent)), "put");
  expect(digest == blake3_digest(kContent), "put returns the BLAKE3 of the content");
  expect(store.has(digest), "has() after put");

  const fs::path blob = expected_blob_path(store.root(), digest);
  expect(fs::is_regular_file(blob), "blob stored at blobs/b3/<2 hex>/<62 hex>");
  expect((fs::status(blob).permissions() & fs::perms::owner_write) == fs::perms::none,
         "published blobs are read-only");

  const std::vector<std::byte> bytes = expect_ok(store.get(digest), "get");
  expect_equal(to_text(bytes), kContent, "get returns the stored bytes");

  std::ifstream stream = expect_ok(store.open(digest), "open");
  const std::string streamed{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};
  expect_equal(streamed, kContent, "open streams the stored bytes");
  expect(store.verify(digest).ok(), "intact blob verifies");
}

void test_put_is_idempotent() {
  TemporaryDirectory scratch(kSuite);
  CasStore store = expect_ok(CasStore::at(scratch.path / "cache"), "open store");
  constexpr std::string_view kContent = "same bytes twice";

  const Blake3Digest first = expect_ok(store.put(as_byte_span(kContent)), "first put");
  const Blake3Digest second = expect_ok(store.put(as_byte_span(kContent)), "second put");
  const fs::path source = scratch.path / "source.bin";
  write_file(source, kContent);
  const Blake3Digest third = expect_ok(store.put_file(source), "put_file of the same bytes");
  expect(first == second && second == third, "identical content yields one digest");

  const CacheUsage usage = expect_ok(store.usage(), "usage");
  expect(usage.blob_count == 1, "identical content is stored once");
  expect(usage.total_bytes == kContent.size(), "usage counts stored bytes");
  expect(count_entries(store.root() / "pending") == 0, "no pending files remain");
}

void test_put_file_round_trip() {
  TemporaryDirectory scratch(kSuite);
  CasStore store = expect_ok(CasStore::at(scratch.path / "cache"), "open store");
  // Larger than one streaming chunk so the copy loop runs more than once.
  const std::string content(3 * 1024 * 1024 + 17, 'x');
  const fs::path source = scratch.path / "large.bin";
  write_file(source, content);

  const Blake3Digest digest = expect_ok(store.put_file(source), "put_file");
  expect(digest == blake3_digest(content), "put_file digests the file content");
  expect(to_text(expect_ok(store.get(digest), "get")) == content, "large blob round-trips");

  expect_cache_error(store.put_file(scratch.path / "missing.bin"), CacheErrorCode::not_found,
                     "put_file of a missing source");
}

void test_corrupt_blob_detected() {
  TemporaryDirectory scratch(kSuite);
  CasStore store = expect_ok(CasStore::at(scratch.path / "cache"), "open store");
  constexpr std::string_view kContent = "original bytes";
  const Blake3Digest digest = expect_ok(store.put(as_byte_span(kContent)), "put");
  const fs::path blob = expected_blob_path(store.root(), digest);

  // Same length, different bytes: only a hash check can notice.
  write_file(blob, "ORIGINAL BYTES");
  expect_cache_error(store.verify(digest), CacheErrorCode::corrupt, "verify tampered blob");
  expect(!store.has(digest), "a corrupt blob is removed");
  expect_cache_error(store.get(digest), CacheErrorCode::not_found, "get after removal");

  expect_ok(store.put(as_byte_span(kContent)), "re-put after corruption");
  write_file(blob, "truncated");
  expect_cache_error(store.get(digest), CacheErrorCode::corrupt, "get of a tampered blob");
  expect_cache_error(store.open(digest), CacheErrorCode::not_found, "open after removal");

  // put() over a corrupt copy repairs it instead of trusting the name.
  expect_ok(store.put(as_byte_span(kContent)), "re-put");
  write_file(blob, "ORIGINAL BYTES");
  expect_ok(store.put(as_byte_span(kContent)), "put over corrupt copy");
  expect(store.verify(digest).ok(), "put repaired the corrupt blob");
}

void test_pending_leftovers_ignored() {
  TemporaryDirectory scratch(kSuite);
  CasStore store = expect_ok(CasStore::at(scratch.path / "cache"), "open store");
  const Blake3Digest stored = expect_ok(store.put(as_byte_span("kept")), "put");

  // Crash leftovers and strays: a half-written pending file, a file whose
  // name is not a digest, and a digest-named file at the wrong depth.
  constexpr std::string_view kHalfWritten = "half-written blob";
  write_file(store.root() / "pending" / "4242-1-0.pending", kHalfWritten);
  write_file(store.root() / "blobs" / "b3" / "ab" / "not-a-digest", "stray");
  write_file(store.root() / "blobs" / "b3" / blake3_hex(blake3_digest("loose")), "loose");

  const CacheUsage usage = expect_ok(store.usage(), "usage");
  expect(usage.blob_count == 1, "only digest-named blobs count");
  expect(usage.total_bytes == std::string_view("kept").size(), "leftovers are not counted");
  expect(!store.has(blake3_digest(kHalfWritten)), "pending bytes are never served");
  expect(store.has(stored), "real blob still present");
}

void test_unwritable_root_is_typed_error() {
  TemporaryDirectory scratch(kSuite);
  const fs::path file_parent = scratch.path / "plain-file";
  write_file(file_parent, "not a directory");
  const auto under_file = CasStore::at(file_parent / "cache");
  expect(!under_file.ok(), "a root below a regular file is reported, not thrown");

  expect_cache_error(CasStore::at(""), CacheErrorCode::invalid_argument, "empty root");

  if (::geteuid() == 0) {
    return;  // root ignores permission bits
  }
  const fs::path locked = scratch.path / "locked";
  fs::create_directories(locked);
  fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);
  expect_cache_error(CasStore::at(locked / "cache"), CacheErrorCode::permission_denied,
                     "root in a read-only directory");

  // An existing layout that has become read-only is caught by the probe.
  const fs::path existing = scratch.path / "existing";
  expect_ok(CasStore::at(existing), "create layout");
  fs::permissions(existing / "pending", fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);
  expect_cache_error(CasStore::at(existing), CacheErrorCode::permission_denied,
                     "read-only pending directory");
}

void test_cache_root_resolution() {
  const CacheRootEnvironment home_only{.home = "/Users/ada"};
  expect(cache_root_for(CachePlatform::macos, home_only) ==
             fs::path("/Users/ada/Library/Caches/org.svp/cache/v1"),
         "macOS default root");
  expect(cache_root_for(CachePlatform::xdg, home_only) == fs::path("/Users/ada/.cache/svp/v1"),
         "XDG fallback root");
  const CacheRootEnvironment xdg{.home = "/home/ada", .xdg_cache_home = "/var/cache/ada"};
  expect(cache_root_for(CachePlatform::xdg, xdg) == fs::path("/var/cache/ada/svp/v1"),
         "XDG_CACHE_HOME root");
  const CacheRootEnvironment relative_xdg{.home = "/home/ada", .xdg_cache_home = "relative"};
  expect(cache_root_for(CachePlatform::xdg, relative_xdg) == fs::path("/home/ada/.cache/svp/v1"),
         "relative XDG_CACHE_HOME is ignored");
  const CacheRootEnvironment windows{.local_app_data = "C:/Users/ada/AppData/Local"};
  expect(cache_root_for(CachePlatform::windows, windows) ==
             fs::path("C:/Users/ada/AppData/Local") / "SVP" / "Cache" / "v1",
         "Windows root");
  const CacheRootEnvironment override_dir{.svp_cache_dir = "/farm/cache", .home = "/Users/ada"};
  for (const CachePlatform platform :
       {CachePlatform::macos, CachePlatform::xdg, CachePlatform::windows}) {
    expect(cache_root_for(platform, override_dir) == fs::path("/farm/cache"),
           "SVP_CACHE_DIR overrides every platform");
  }
  expect(!cache_root_for(CachePlatform::macos, {}).has_value(), "no HOME, no root");
  expect(!cache_root_for(CachePlatform::macos, CacheRootEnvironment{.home = ""}).has_value(),
         "empty HOME counts as unset");
}

}  // namespace

int main() {
  return run_tests(kSuite, {
                               {"put_get_round_trip", test_put_get_round_trip},
                               {"put_is_idempotent", test_put_is_idempotent},
                               {"put_file_round_trip", test_put_file_round_trip},
                               {"corrupt_blob_detected", test_corrupt_blob_detected},
                               {"pending_leftovers_ignored", test_pending_leftovers_ignored},
                               {"unwritable_root_is_typed_error",
                                test_unwritable_root_is_typed_error},
                               {"cache_root_resolution", test_cache_root_resolution},
                           });
}
