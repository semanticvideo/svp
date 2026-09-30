#include "svp/builder/process_resources.hpp"

#include <sys/resource.h>

namespace svp::builder {
namespace {

std::int64_t timeval_ms(const timeval& value) {
  return static_cast<std::int64_t>(value.tv_sec) * 1000 +
         static_cast<std::int64_t>(value.tv_usec) / 1000;
}

// getrusage reports ru_maxrss in bytes on Darwin and in kilobytes on Linux.
std::int64_t max_rss_bytes(const rusage& usage) {
#if defined(__APPLE__)
  return static_cast<std::int64_t>(usage.ru_maxrss);
#else
  constexpr std::int64_t kLinuxMaxRssUnitBytes = 1024;
  return static_cast<std::int64_t>(usage.ru_maxrss) * kLinuxMaxRssUnitBytes;
#endif
}

}  // namespace

std::optional<ProcessResourceSample> sample_process_resources() {
  rusage usage{};
  rusage children{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0 ||
      ::getrusage(RUSAGE_CHILDREN, &children) != 0) {
    return std::nullopt;
  }
  return ProcessResourceSample{
      .user_cpu_ms = timeval_ms(usage.ru_utime),
      .system_cpu_ms = timeval_ms(usage.ru_stime),
      .children_user_cpu_ms = timeval_ms(children.ru_utime),
      .children_system_cpu_ms = timeval_ms(children.ru_stime),
      .peak_rss_bytes = max_rss_bytes(usage),
  };
}

}  // namespace svp::builder
