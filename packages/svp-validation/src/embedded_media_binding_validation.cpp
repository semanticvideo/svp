#include "embedded_media_binding_validation.hpp"

#include "svp/package/package_layout.hpp"

#include <blake3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

namespace svp::validation {
namespace {

std::optional<std::string> expected_full_file_hash(
    const std::filesystem::path& container_path) {
  const auto entry = svp::package::read_package_entry(container_path, "media_binding.json");
  if (!entry.has_value()) {
    return std::nullopt;
  }
  const auto document = nlohmann::json::parse(entry.value(), nullptr, false);
  if (!document.is_object() || !document.contains("bindings") ||
      !document["bindings"].is_array()) {
    return std::nullopt;
  }
  for (const auto& binding : document["bindings"]) {
    if (!binding.is_object() || binding.value("media_role", "") != "primary_source" ||
        !binding.contains("identity") || !binding["identity"].is_object()) {
      continue;
    }
    const auto& identity = binding["identity"];
    if (!identity.contains("full_file_blake3") ||
        !identity["full_file_blake3"].is_object()) {
      return std::nullopt;
    }
    const auto& hash = identity["full_file_blake3"];
    if (hash.value("state", "") != "present" ||
        !hash.contains("value") || !hash["value"].is_string()) {
      return std::nullopt;
    }
    return hash["value"].get<std::string>();
  }
  return std::nullopt;
}

bool hash_range(std::ifstream& input, blake3_hasher& hasher,
                std::uint64_t offset, std::uint64_t size) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max())) {
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  std::vector<char> buffer(1024 * 1024);
  while (size > 0 && input) {
    const auto chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(size, buffer.size()));
    input.read(buffer.data(), chunk);
    if (input.gcount() != chunk) {
      return false;
    }
    blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(chunk));
    size -= static_cast<std::uint64_t>(chunk);
  }
  return size == 0;
}

std::optional<std::string> clean_container_hash(
    const std::filesystem::path& path,
    const svp::package::EmbeddedSvpiInfo& embedding) {
  std::error_code error;
  const auto file_size = std::filesystem::file_size(path, error);
  if (error || embedding.box_offset > file_size ||
      embedding.box_size > file_size - embedding.box_offset) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  if (!hash_range(input, hasher, 0, embedding.box_offset)) {
    return std::nullopt;
  }
  const auto suffix_offset = embedding.box_offset + embedding.box_size;
  if (!hash_range(input, hasher, suffix_offset, file_size - suffix_offset)) {
    return std::nullopt;
  }
  std::array<std::uint8_t, 32> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  std::ostringstream hex;
  hex << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    hex << std::setw(2) << static_cast<unsigned>(byte);
  }
  return "blake3:" + hex.str();
}

}  // namespace

void add_embedded_media_binding_finding(
    ValidationReport& report,
    const ValidationCodeRegistry& registry,
    const std::filesystem::path& container_path,
    const svp::package::EmbeddedSvpiInfo& embedding) {
  const auto expected = expected_full_file_hash(container_path);
  if (!expected.has_value()) {
    return;
  }
  const auto actual = clean_container_hash(container_path, embedding);
  if (!actual.has_value() || *actual != *expected) {
    add_finding(report, make_finding(
        registry, kCodeIsoBmffSvpiMediaBindingMismatch, "/media_binding.json",
        "Embedded SVPI full-file BLAKE3 does not match the clean container bytes."));
  }
}

}  // namespace svp::validation
