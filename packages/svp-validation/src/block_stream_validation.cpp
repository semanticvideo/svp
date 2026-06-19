#include "block_stream_validation.hpp"

#include "svp/blocks/block_stream.hpp"
#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace svp::validation {
namespace {

constexpr std::string_view kManifestEntry = "manifest.json";
constexpr std::string_view kDepthIndexEntry = "spatial/depth.index.jsonl";
constexpr std::string_view kDepthBlocksEntry = "spatial/depth.blocks.svpdz";
constexpr std::string_view kMaskIndexEntry = "spatial/masks.index.jsonl";
constexpr std::string_view kMaskBlocksEntry = "spatial/masks.blocks.svpmz";
constexpr std::string_view kEmbeddingBlocksEntry = "embeddings/embeddings.blocks.svpez";

struct ZipDeleter {
  void operator()(zip_t* archive) const noexcept {
    if (archive != nullptr) {
      zip_discard(archive);
    }
  }
};

struct ZipFileDeleter {
  void operator()(zip_file_t* file) const noexcept {
    if (file != nullptr) {
      zip_fclose(file);
    }
  }
};

std::string package_entry_path(std::string_view entry) {
  return "/" + std::string{entry};
}

std::string zip_error_message(int error_code) {
  zip_error_t error;
  zip_error_init_with_code(&error, error_code);
  std::string message = zip_error_strerror(&error);
  zip_error_fini(&error);
  return message;
}

std::optional<svp::blocks::RasterExtent> read_manifest_raster(
    const std::filesystem::path& package_path,
    const svp::package::PackageLayout& layout) {
  if (!layout.has_entry(std::string{kManifestEntry})) {
    return std::nullopt;
  }

  const auto manifest_result =
      svp::package::read_package_entry(package_path, std::string{kManifestEntry});
  if (!manifest_result.has_value()) {
    return std::nullopt;
  }

  try {
    const auto manifest = nlohmann::json::parse(manifest_result.value());
    const auto& raster = manifest.at("canonical_analysis_raster");
    const auto width = raster.at("width").get<std::uint32_t>();
    const auto height = raster.at("height").get<std::uint32_t>();
    if (width == 0 || height == 0) {
      return std::nullopt;
    }
    return svp::blocks::RasterExtent{.width = width, .height = height};
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

void add_missing_required_entry(ValidationReport& report,
                                const ValidationCodeRegistry& registry,
                                std::string_view code,
                                std::string_view entry) {
  add_finding(report, make_finding(registry, code, package_entry_path(entry),
                                   "Required SVPB package entry is absent."));
}

class ZipEntryReader {
 public:
  ZipEntryReader(const std::filesystem::path& package_path, std::string_view entry) {
    int error_code = ZIP_ER_OK;
    archive_.reset(zip_open(package_path.string().c_str(), ZIP_RDONLY, &error_code));
    if (!archive_) {
      throw std::runtime_error(zip_error_message(error_code));
    }

    zip_stat_init(&stat_);
    if (zip_stat(archive_.get(), std::string{entry}.c_str(), 0, &stat_) != 0) {
      throw std::runtime_error(zip_strerror(archive_.get()));
    }

    file_.reset(zip_fopen(archive_.get(), std::string{entry}.c_str(), 0));
    if (!file_) {
      throw std::runtime_error(zip_strerror(archive_.get()));
    }
  }

  [[nodiscard]] std::uint64_t size() const {
    if ((stat_.valid & ZIP_STAT_SIZE) == 0) {
      throw std::runtime_error("ZIP entry size is unavailable");
    }
    return stat_.size;
  }

  [[nodiscard]] bool is_stored() const noexcept {
    return (stat_.valid & ZIP_STAT_COMP_METHOD) != 0 && stat_.comp_method == ZIP_CM_STORE;
  }

  bool read_exact(std::byte* output, std::size_t byte_count, std::string& error_message) {
    std::size_t offset = 0;
    while (offset < byte_count) {
      const auto bytes_read =
          zip_fread(file_.get(), output + offset, byte_count - offset);
      if (bytes_read < 0) {
        error_message = zip_file_strerror(file_.get());
        return false;
      }
      if (bytes_read == 0) {
        error_message = "ZIP entry ended before declared SVPB block range.";
        return false;
      }
      offset += static_cast<std::size_t>(bytes_read);
    }
    return true;
  }

 private:
  std::unique_ptr<zip_t, ZipDeleter> archive_;
  std::unique_ptr<zip_file_t, ZipFileDeleter> file_;
  zip_stat_t stat_{};
};

std::string_view code_for_issue(svp::blocks::IssueKind issue) noexcept {
  switch (issue) {
    case svp::blocks::IssueKind::invalid_header:
      return kCodeInvalidBlockHeader;
    case svp::blocks::IssueKind::forbidden_block_type:
      return kCodeForbiddenBlockType;
    case svp::blocks::IssueKind::raster_extent_mismatch:
      return kCodeRasterExtentMismatch;
  }

  return kCodeInvalidBlockHeader;
}

void add_parser_findings(ValidationReport& report,
                         const ValidationCodeRegistry& registry,
                         std::string_view entry,
                         const svp::blocks::ParseResult& result) {
  for (const auto& issue : result.issues) {
    add_finding(report, make_finding(registry, code_for_issue(issue.kind),
                                     package_entry_path(entry) + "@" +
                                         std::to_string(issue.offset),
                                     issue.message));
  }
}

void validate_block_stream_entry(ValidationReport& report,
                                 const ValidationCodeRegistry& registry,
                                 const std::filesystem::path& package_path,
                                 std::string_view entry,
                                 const svp::blocks::ParseOptions& options) {
  try {
    ZipEntryReader reader{package_path, entry};
    if (!reader.is_stored()) {
      add_finding(report,
                  make_finding(registry, kTempCodeBlockEntryNotStored,
                               package_entry_path(entry),
                               "SVPB block stream package entries must use ZIP STORE."));
    }

    const auto result = svp::blocks::parse_block_stream(
        reader.size(), options,
        [&](std::byte* output, std::size_t byte_count, std::string& error_message) {
          return reader.read_exact(output, byte_count, error_message);
        });
    add_parser_findings(report, registry, entry, result);
  } catch (const std::exception& error) {
    add_finding(report, make_finding(registry, kCodeInvalidBlockHeader,
                                     package_entry_path(entry), error.what()));
  }
}

}  // namespace

void add_block_stream_findings(ValidationReport& report,
                               const ValidationCodeRegistry& registry,
                               const std::filesystem::path& package_path,
                               const svp::package::PackageLayout& layout) {
  const auto raster = read_manifest_raster(package_path, layout);

  if (!layout.has_entry(std::string{kDepthIndexEntry})) {
    add_missing_required_entry(report, registry, kCodeMissingDepth, kDepthIndexEntry);
  }
  if (!layout.has_entry(std::string{kDepthBlocksEntry})) {
    add_missing_required_entry(report, registry, kCodeMissingDepth, kDepthBlocksEntry);
  } else {
    svp::blocks::ParseOptions options;
    options.required_block_type = svp::blocks::BlockType::depth;
    options.required_raster_extent = raster;
    validate_block_stream_entry(report, registry, package_path, kDepthBlocksEntry, options);
  }

  if (!layout.has_entry(std::string{kMaskIndexEntry})) {
    add_missing_required_entry(report, registry, kCodeMissingMasks, kMaskIndexEntry);
  }
  if (!layout.has_entry(std::string{kMaskBlocksEntry})) {
    add_missing_required_entry(report, registry, kCodeMissingMasks, kMaskBlocksEntry);
  } else {
    svp::blocks::ParseOptions options;
    options.required_block_type = svp::blocks::BlockType::mask;
    options.required_raster_extent = raster;
    options.allow_empty = true;
    validate_block_stream_entry(report, registry, package_path, kMaskBlocksEntry, options);
  }

  if (layout.has_entry(std::string{kEmbeddingBlocksEntry})) {
    svp::blocks::ParseOptions options;
    options.required_block_type = svp::blocks::BlockType::embedding;
    validate_block_stream_entry(report, registry, package_path, kEmbeddingBlocksEntry,
                                options);
  }
}

}  // namespace svp::validation
