#include "svp/exec/runtime_release.hpp"

#include "json_fields.hpp"
#include "svp/exec/canonical_json.hpp"
#include "svp/exec/exec_error.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace svp::exec {
namespace {

constexpr std::string_view kRecordPath = "runtime_release";

}  // namespace

std::string encode_runtime_release_record(std::uint64_t release_stamp) {
  return encode_canonical_json(nlohmann::json{{"release_stamp", release_stamp},
                                              {"schema", std::string(kRuntimeReleaseSchema)}});
}

std::uint64_t decode_runtime_release_record(std::string_view bytes) {
  const nlohmann::json value = decode_canonical_json(bytes);
  detail::require_object(value, kRecordPath);
  detail::reject_unknown_fields(value, {"release_stamp", "schema"}, kRecordPath);
  const std::string schema = detail::required_string(value, "schema", kRecordPath);
  if (schema != kRuntimeReleaseSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "unsupported runtime release schema `" + schema + "`");
  }
  return detail::required_unsigned(value, "release_stamp", kRecordPath);
}

RuntimeRelease runtime_release_of(const RuntimeManifest& manifest,
                                  const std::filesystem::path& runtime_dir) {
  RuntimeRelease release{.runtime_id = compute_runtime_id(manifest), .release_stamp = {}};
  for (const RuntimeManifestFile& file : manifest.files) {
    if (file.path != kRuntimeReleasePath) {
      continue;
    }
    std::ifstream input(runtime_dir / file.path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    if (!input.bad() && bytes.size() == file.size_bytes && blake3_digest(bytes) == file.blake3) {
      try {
        release.release_stamp = decode_runtime_release_record(bytes);
      } catch (const ExecError&) {
        release.release_stamp.reset();
      }
    }
    break;
  }
  return release;
}

bool is_newer_release(const RuntimeRelease& candidate, const RuntimeRelease& current) noexcept {
  if (!candidate.release_stamp) {
    return false;
  }
  if (!current.release_stamp) {
    return true;
  }
  if (*candidate.release_stamp != *current.release_stamp) {
    return *candidate.release_stamp > *current.release_stamp;
  }
  // std::array compares element-wise; elements are std::uint8_t.
  return candidate.runtime_id > current.runtime_id;
}

std::uint64_t release_stamp_now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace svp::exec
