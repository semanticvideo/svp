#include "svp/exec/source_fingerprint.hpp"

#include "durable_io.hpp"
#include "svp/exec/journal_error.hpp"

#include <sys/stat.h>

namespace svp::exec {
namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

std::optional<std::int64_t> mtime_ns(const std::filesystem::path& path) {
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0) {
    return std::nullopt;
  }
#if defined(__APPLE__)
  const struct timespec& modified = status.st_mtimespec;
#else
  const struct timespec& modified = status.st_mtim;
#endif
  return static_cast<std::int64_t>(modified.tv_sec) * kNanosecondsPerSecond + modified.tv_nsec;
}

}  // namespace

SourceFingerprintRecord fingerprint_source(std::string source_id,
                                           const std::filesystem::path& path) {
  detail::FileDigest digest;
  if (const auto error = detail::hash_file(path, digest)) {
    throw JournalError(JournalErrorCode::io_error,
                       "fingerprint " + path.string() + ": " + error.message());
  }
  return SourceFingerprintRecord{.source_id = std::move(source_id),
                                 .path = path.string(),
                                 .size_bytes = digest.bytes,
                                 .mtime_ns = mtime_ns(path),
                                 .blake3 = digest.digest};
}

}  // namespace svp::exec
