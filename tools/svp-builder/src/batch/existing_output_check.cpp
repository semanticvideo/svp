#include "existing_output_check.hpp"

#include "../interlace/interlace_batch_internal.hpp"

#include "svp/validation/report.hpp"
#include "svp/validation/validator.hpp"

#include <zip.h>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>
#include <fstream>
#include <memory>
#include <string_view>

namespace svp::builder::batch {
namespace {

constexpr std::string_view kOriginalMediaPrefix = "media/original/";
// Bytes compared per read: a multi-gigabyte source takes a few thousand
// reads, while the two buffers stay a few MiB.
constexpr std::size_t kCompareChunkBytes = 4U * 1024U * 1024U;

struct ZipCloser {
  void operator()(zip_t* archive) const noexcept { zip_discard(archive); }
};
struct ZipFileCloser {
  void operator()(zip_file_t* file) const noexcept { zip_fclose(file); }
};

// True when the package's one original media entry is `source`, byte for
// byte.
bool holds_source(const std::filesystem::path& package, const std::filesystem::path& source,
                  std::string& reason) {
  int error = ZIP_ER_OK;
  const std::unique_ptr<zip_t, ZipCloser> archive(
      zip_open(package.string().c_str(), ZIP_RDONLY, &error));
  if (!archive) {
    reason = "cannot open the package";
    return false;
  }
  std::optional<zip_uint64_t> entry;
  zip_stat_t stat;
  const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
  for (zip_int64_t index = 0; index < count; ++index) {
    if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(index), 0, &stat) != 0 ||
        stat.name == nullptr) {
      continue;
    }
    const std::string_view name(stat.name);
    if (name.rfind(kOriginalMediaPrefix, 0) == 0 && name.back() != '/') {
      if (entry) {
        reason = "the package holds more than one original media file";
        return false;
      }
      entry = static_cast<zip_uint64_t>(index);
    }
  }
  if (!entry || zip_stat_index(archive.get(), *entry, 0, &stat) != 0) {
    reason = "the package holds no original media";
    return false;
  }
  std::error_code size_error;
  if (stat.size != std::filesystem::file_size(source, size_error) || size_error) {
    reason = "the package's original media is not this source";
    return false;
  }
  const std::unique_ptr<zip_file_t, ZipFileCloser> packaged(
      zip_fopen_index(archive.get(), *entry, 0));
  std::ifstream original(source, std::ios::binary);
  if (!packaged || !original) {
    reason = "cannot read the package's original media or the source";
    return false;
  }
  std::vector<char> left(kCompareChunkBytes);
  std::vector<char> right(kCompareChunkBytes);
  zip_uint64_t remaining = stat.size;
  while (remaining > 0) {
    const std::size_t want =
        static_cast<std::size_t>(std::min<zip_uint64_t>(remaining, kCompareChunkBytes));
    std::size_t got = 0;
    while (got < want) {
      const zip_int64_t read = zip_fread(packaged.get(), left.data() + got, want - got);
      if (read <= 0) {
        reason = "cannot read the package's original media";
        return false;
      }
      got += static_cast<std::size_t>(read);
    }
    original.read(right.data(), static_cast<std::streamsize>(want));
    if (static_cast<std::size_t>(original.gcount()) != want ||
        !std::equal(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(want),
                    right.begin())) {
      reason = "the package's original media is not this source";
      return false;
    }
    remaining -= want;
  }
  return true;
}

}  // namespace

bool existing_output_is_complete(VideoOutputFormat format, const std::filesystem::path& output,
                                 const std::filesystem::path& source, std::string& reason) {
  switch (format) {
    case VideoOutputFormat::svp: {
      const svp::validation::ValidationReport report =
          svp::validation::validate_package(output, svp::validation::ValidatorOptions{});
      if (svp::validation::exit_code(report) != 0) {
        reason = "the package does not validate";
        return false;
      }
      return holds_source(output, source, reason);
    }
    case VideoOutputFormat::svpi:
      return check_svpi_valid_and_bound(output, source, {}, reason);
    case VideoOutputFormat::embedded_svpi:
      return check_embedded_batch_artifact(output, source, {}, reason);
  }
  return false;
}

}  // namespace svp::builder::batch
