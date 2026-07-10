#include "svp/builder/embedded_svpi_transport.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>

#include <unistd.h>

namespace svp::builder {

namespace {

class ManagedSvpiDirectory {
 public:
  ManagedSvpiDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "svp-embedded-build-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    if (mkdtemp(writable.data()) != nullptr) {
      path_ = writable.data();
    }
  }

  ~ManagedSvpiDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

}  // namespace

EmbeddedTransportBuildResult build_embedded_svpi_transport(
    const EmbeddedTransportBuildOptions& options) {
  EmbeddedTransportBuildResult result;
  result.output_path = options.output_path;

  ManagedSvpiDirectory temporary;
  if (temporary.path().empty()) {
    result.error_message =
        "Unable to create managed temporary directory for canonical SVPI.";
    return result;
  }

  auto create_options = options.svpi_options;
  const auto temporary_svpi = temporary.path() / "semantic-package.svpi";
  create_options.output_path = temporary_svpi.string();
  const auto svpi = interlace_create(create_options);
  if (!svpi.success) {
    result.error_message = "Canonical SVPI build failed: " +
                           svpi.error_message;
    return result;
  }

  EmbeddedTransportEmbedOptions embed;
  embed.container_path = create_options.source_path;
  embed.svpi_path = temporary_svpi;
  embed.output_path = options.output_path;
  embed.ffprobe_path = create_options.ffprobe_path;
  embed.overwrite_output = options.overwrite_output;
  embed.progress_sink = create_options.progress_sink;
  const auto embedded = embed_svpi_transport(embed);
  result.validation_report = embedded.validation_report;
  if (!embedded.success) {
    result.error_message = "Embedded SVPI Transport build failed: " +
                           embedded.error_message;
    return result;
  }

  result.success = true;
  return result;
}

}  // namespace svp::builder
