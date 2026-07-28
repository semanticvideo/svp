#include "svp/models/hash.hpp"

#include "canonical_cbor.hpp"
#include "canonical_control_json.hpp"
#include "svp/models/error.hpp"

#include <algorithm>
#include <array>
#include <blake3.h>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace svp::models {
namespace {

constexpr std::string_view kBundleDigestDomain = "SVP_MODEL_BUNDLE_BLAKE3_V1";
constexpr std::string_view kManifestFileName = "model.svpmodel.json";
constexpr std::string_view kLockFileName = "model-lock.json";
constexpr std::array<std::string_view, 4> kRequiredRootFiles = {
    kManifestFileName, kLockFileName, "LICENSE", "NOTICE"};
constexpr std::size_t kBundleDigestHexLength = 64;
constexpr std::size_t kBundleIdDigestPrefixLength = 12;

struct BundleFile {
  std::string relative_path;
  std::filesystem::path physical_path;
};

std::string to_lower_hex(const std::array<uint8_t, BLAKE3_OUT_LEN>& digest) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return stream.str();
}

[[noreturn]] void throw_bundle_io_error(const std::filesystem::path& path,
                                        std::string_view operation) {
  throw ModelError(ModelErrorCode::io_error,
                   std::string(operation) + ": " + path.string());
}

bool is_valid_utf8(std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7fU) {
      continuation_count = 0;
      code_point = first;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      continuation_count = 1;
      code_point = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation_count = 2;
      code_point = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation_count = 3;
      code_point = first & 0x07U;
    } else {
      return false;
    }

    if (offset + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto continuation =
          static_cast<unsigned char>(value[offset + index]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }

    if ((continuation_count == 2 && code_point < 0x800U) ||
        (continuation_count == 3 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) ||
        code_point > 0x10ffffU) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool unsigned_byte_less(std::string_view left, std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char left_byte, char right_byte) {
        return static_cast<unsigned char>(left_byte) <
               static_cast<unsigned char>(right_byte);
      });
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw_bundle_io_error(path, "could not open model bundle control file");
  }
  const std::streampos end = file.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) >
                     std::numeric_limits<std::size_t>::max()) {
    throw_bundle_io_error(path, "could not size model bundle control file");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  file.seekg(0);
  if (!bytes.empty()) {
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  if (!file) {
    throw_bundle_io_error(path, "could not read model bundle control file");
  }
  return bytes;
}

void validate_logical_path(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos || !is_valid_utf8(path)) {
    throw ModelError(ModelErrorCode::schema_error,
                     "model bundle manifest contains a non-canonical path: " +
                         std::string(path));
  }
  std::size_t begin = 0;
  while (begin < path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::string_view component = path.substr(
        begin, end == std::string_view::npos ? path.size() - begin : end - begin);
    if (component.empty() || component == "." || component == "..") {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle manifest contains a non-canonical path: " +
                           std::string(path));
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
}

std::filesystem::path physical_path_for(const std::filesystem::path& root,
                                        std::string_view logical_path) {
  const std::u8string utf8(
      reinterpret_cast<const char8_t*>(logical_path.data()), logical_path.size());
  return root / std::filesystem::path(utf8);
}

std::vector<BundleFile> expected_bundle_files(
    const std::filesystem::path& bundle_root) {
  std::vector<BundleFile> files;
  std::set<std::string> fixed_paths;
  for (const std::string_view path : kRequiredRootFiles) {
    fixed_paths.emplace(path);
    files.push_back({std::string(path), physical_path_for(bundle_root, path)});
  }

  const auto manifest = detail::parse_canonical_control_json(
      read_file_bytes(bundle_root / kManifestFileName),
      bundle_root / kManifestFileName);
  if (!manifest.is_object() || !manifest.contains("files") ||
      !manifest["files"].is_array()) {
    throw ModelError(ModelErrorCode::schema_error,
                     "model bundle manifest must contain a files array");
  }
  std::set<std::string> declared_paths;
  for (const auto& entry : manifest["files"]) {
    if (!entry.is_object() || !entry.contains("path") ||
        !entry["path"].is_string()) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle manifest files entries require string paths");
    }
    const std::string path = entry["path"].get<std::string>();
    validate_logical_path(path);
    if (!declared_paths.insert(path).second) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle manifest contains a duplicate path: " + path);
    }
    if (path == "LICENSE" || path == "NOTICE") {
      const std::string expected_role = path == "LICENSE" ? "license" : "notice";
      if (!entry.contains("role") || !entry["role"].is_string() ||
          entry["role"].get<std::string>() != expected_role) {
        throw ModelError(ModelErrorCode::schema_error,
                         "model bundle manifest path " + path +
                             " requires role " + expected_role);
      }
    }
    if (fixed_paths.contains(path)) {
      continue;
    }
    files.push_back({path, physical_path_for(bundle_root, path)});
  }
  return files;
}

std::vector<BundleFile> enumerate_bundle_files(
    const std::filesystem::path& bundle_root) {
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(bundle_root, error);
  if (error || !std::filesystem::is_directory(root_status)) {
    throw_bundle_io_error(bundle_root, "model bundle root is not a directory");
  }

  std::vector<BundleFile> files = expected_bundle_files(bundle_root);
  for (const BundleFile& file : files) {
    const auto status = std::filesystem::symlink_status(file.physical_path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle logical path does not resolve to a regular file: " +
                           file.relative_path);
    }
  }
  for (std::size_t left = 0; left < files.size(); ++left) {
    for (std::size_t right = left + 1; right < files.size(); ++right) {
      error.clear();
      if (std::filesystem::equivalent(files[left].physical_path,
                                      files[right].physical_path, error) &&
          !error) {
        throw ModelError(ModelErrorCode::schema_error,
                         "model bundle logical paths alias the same file: " +
                             files[left].relative_path + " and " +
                             files[right].relative_path);
      }
    }
  }

  std::size_t actual_file_count = 0;
  std::filesystem::recursive_directory_iterator iterator(bundle_root, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    throw_bundle_io_error(bundle_root, "could not enumerate model bundle");
  }
  while (iterator != end) {
    const auto path = iterator->path();
    const auto status = iterator->symlink_status(error);
    if (error) {
      throw_bundle_io_error(path, "could not inspect model bundle entry");
    }
    if (std::filesystem::is_symlink(status) ||
        (!std::filesystem::is_directory(status) &&
         !std::filesystem::is_regular_file(status))) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle contains an unsupported file type: " +
                           path.string());
    }
    if (std::filesystem::is_regular_file(status)) {
      ++actual_file_count;
      std::size_t matches = 0;
      for (const BundleFile& expected : files) {
        error.clear();
        if (std::filesystem::equivalent(path, expected.physical_path, error) &&
            !error) {
          ++matches;
        }
      }
      if (matches != 1) {
        throw ModelError(ModelErrorCode::schema_error,
                         "model bundle contains an undeclared or aliased file: " +
                             path.string());
      }
    }
    iterator.increment(error);
    if (error) {
      throw_bundle_io_error(path, "could not enumerate model bundle");
    }
  }
  if (actual_file_count != files.size()) {
    throw ModelError(ModelErrorCode::schema_error,
                     "model bundle file inventory does not match logical paths");
  }
  std::sort(files.begin(), files.end(), [](const BundleFile& left,
                                           const BundleFile& right) {
    return unsigned_byte_less(left.relative_path, right.relative_path);
  });
  return files;
}

std::string zero_bundle_hash() {
  return "blake3:" + std::string(kBundleDigestHexLength, '0');
}

std::string zero_bundle_id(const nlohmann::json& value,
                           std::string_view source_name) {
  if (!value.contains("model_id") || !value["model_id"].is_string() ||
      !value.contains("model_version") || !value["model_version"].is_string()) {
    throw ModelError(ModelErrorCode::schema_error,
                     std::string(source_name) +
                         " must contain string model_id and model_version fields");
  }
  return value["model_id"].get<std::string>() + "@" +
         value["model_version"].get<std::string>() + "+blake3_" +
         std::string(kBundleIdDigestPrefixLength, '0');
}

std::vector<std::uint8_t> projected_control_bytes(const BundleFile& file) {
  nlohmann::json value = detail::parse_canonical_control_json(
      read_file_bytes(file.physical_path), file.physical_path);
  if (file.relative_path == kManifestFileName) {
    if (!value.is_object()) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model bundle manifest must be a JSON object");
    }
    value["bundle_blake3"] = zero_bundle_hash();
    value["model_bundle_id"] = zero_bundle_id(value, kManifestFileName);
  } else {
    if (!value.is_object() || !value.contains("models") ||
        !value["models"].is_array()) {
      throw ModelError(ModelErrorCode::schema_error,
                       "model-lock.json must contain a models array");
    }
    for (nlohmann::json& model : value["models"]) {
      if (!model.is_object()) {
        throw ModelError(ModelErrorCode::schema_error,
                         "model-lock.json models entries must be objects");
      }
      model["bundle_blake3"] = zero_bundle_hash();
      model["model_bundle_id"] = zero_bundle_id(model, kLockFileName);
    }
  }
  return detail::encode_canonical_control_cbor(value);
}

void update_u64(blake3_hasher& hasher, std::uint64_t value) {
  std::array<std::uint8_t, sizeof(value)> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - index - 1] = static_cast<std::uint8_t>(value & 0xffU);
    value >>= 8U;
  }
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
}

void update_regular_file(blake3_hasher& hasher,
                         const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw_bundle_io_error(path, "could not open model bundle file");
  }
  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = file.gcount();
    if (count > 0) {
      blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (file.bad()) {
    throw_bundle_io_error(path, "failed while reading model bundle file");
  }
}

}  // namespace

std::string blake3_hex_for_model_bundle(
    const std::filesystem::path& bundle_root) {
  const std::vector<BundleFile> files = enumerate_bundle_files(bundle_root);

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, kBundleDigestDomain.data(),
                       kBundleDigestDomain.size());
  constexpr std::uint8_t zero = 0;
  blake3_hasher_update(&hasher, &zero, sizeof(zero));
  update_u64(hasher, static_cast<std::uint64_t>(files.size()));

  for (const BundleFile& file : files) {
    constexpr std::uint8_t file_record = 0x01;
    blake3_hasher_update(&hasher, &file_record, sizeof(file_record));
    update_u64(hasher, static_cast<std::uint64_t>(file.relative_path.size()));
    blake3_hasher_update(&hasher, file.relative_path.data(),
                         file.relative_path.size());

    if (file.relative_path == kManifestFileName ||
        file.relative_path == kLockFileName) {
      const std::vector<std::uint8_t> content = projected_control_bytes(file);
      update_u64(hasher, static_cast<std::uint64_t>(content.size()));
      blake3_hasher_update(&hasher, content.data(), content.size());
      continue;
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file.physical_path, error);
    if (error || size > std::numeric_limits<std::uint64_t>::max()) {
      throw_bundle_io_error(file.physical_path,
                            "could not size model bundle file");
    }
    update_u64(hasher, static_cast<std::uint64_t>(size));
    update_regular_file(hasher, file.physical_path);
  }

  std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return to_lower_hex(digest);
}

}  // namespace svp::models
