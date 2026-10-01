#include "svp/exec/runtime_components.hpp"

#include "json_fields.hpp"
#include "svp/exec/exec_error.hpp"

#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>

namespace svp::exec {
namespace {

constexpr std::string_view kComponentsPath = "runtime_components";
// components.json spells digests the way `svp-models-tool hash` prints them.
constexpr std::string_view kComponentsDigestPrefix = "blake3:";

Blake3Digest component_digest(const nlohmann::json& entry, std::string_view path) {
  const std::string value = detail::required_string(entry, "blake3", path);
  if (value.rfind(kComponentsDigestPrefix, 0) == 0) {
    if (const auto digest =
            parse_blake3_hex(std::string_view(value).substr(kComponentsDigestPrefix.size()))) {
      return *digest;
    }
  }
  throw ExecError(ExecErrorCode::invalid_digest,
                  detail::child_path(path, "blake3") + " must be blake3:<64 hex>: `" +
                      value + "`");
}

}  // namespace

const RuntimeComponentFile* RuntimeComponents::find(std::string_view path) const noexcept {
  for (const RuntimeComponentFile& file : files) {
    if (file.path == path) return &file;
  }
  return nullptr;
}

RuntimeComponents parse_runtime_components(std::string_view bytes) {
  nlohmann::json value;
  try {
    value = nlohmann::json::parse(bytes);
  } catch (const nlohmann::json::parse_error& error) {
    throw ExecError(ExecErrorCode::invalid_json,
                    std::string("runtime components record: ") + error.what());
  }
  detail::require_object(value, kComponentsPath);
  const std::string schema = detail::required_string(value, "schema", kComponentsPath);
  if (schema != kRuntimeComponentsSchema) {
    throw ExecError(ExecErrorCode::invalid_value,
                    "unsupported runtime components schema `" + schema + "`");
  }

  RuntimeComponents result;
  result.arch = detail::required_string(value, "arch", kComponentsPath);
  result.macos_deployment_target =
      detail::required_string(value, "macos_deployment_target", kComponentsPath);
  const nlohmann::json& components =
      detail::required_array(value, "components", kComponentsPath);
  for (std::size_t index = 0; index < components.size(); ++index) {
    const std::string component_path = detail::child_path(kComponentsPath, "components") +
                                       "[" + std::to_string(index) + "]";
    const nlohmann::json& component = components[index];
    detail::require_object(component, component_path);
    const std::string name = detail::required_string(component, "component", component_path);
    const nlohmann::json& files = detail::required_array(component, "files", component_path);
    for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
      const std::string file_path = detail::child_path(component_path, "files") + "[" +
                                    std::to_string(file_index) + "]";
      const nlohmann::json& entry = files[file_index];
      detail::require_object(entry, file_path);
      result.files.push_back(RuntimeComponentFile{
          .component = name,
          .path = detail::required_string(entry, "path", file_path),
          .blake3 = component_digest(entry, file_path),
          .size_bytes = detail::required_unsigned(entry, "size_bytes", file_path),
      });
    }
  }
  return result;
}

RuntimeComponents load_runtime_components(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                            "cannot open runtime components record " + path.string());
  }
  const std::string bytes{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
  if (input.bad()) {
    throw std::system_error(std::make_error_code(std::errc::io_error),
                            "cannot read runtime components record " + path.string());
  }
  return parse_runtime_components(bytes);
}

}  // namespace svp::exec
