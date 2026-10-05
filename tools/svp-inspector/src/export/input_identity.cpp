#include "input_identity.hpp"

#include <sys/stat.h>

namespace package_export {

std::optional<InputIdentity> read_input_identity(
    const std::filesystem::path& path) {
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0 || status.st_size < 0) {
    return std::nullopt;
  }
  InputIdentity identity;
  identity.device = static_cast<std::uint64_t>(status.st_dev);
  identity.inode = static_cast<std::uint64_t>(status.st_ino);
  identity.size_bytes = static_cast<std::uint64_t>(status.st_size);
  identity.is_regular_file = S_ISREG(status.st_mode);
#if defined(__APPLE__)
  identity.modified_seconds = status.st_mtimespec.tv_sec;
  identity.modified_nanoseconds = status.st_mtimespec.tv_nsec;
  identity.changed_seconds = status.st_ctimespec.tv_sec;
  identity.changed_nanoseconds = status.st_ctimespec.tv_nsec;
#else
  identity.modified_seconds = status.st_mtim.tv_sec;
  identity.modified_nanoseconds = status.st_mtim.tv_nsec;
  identity.changed_seconds = status.st_ctim.tv_sec;
  identity.changed_nanoseconds = status.st_ctim.tv_nsec;
#endif
  return identity;
}

}  // namespace package_export
