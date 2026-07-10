#include "svp/package/embedded_svpi.hpp"
#include "svp/validation/code_registry.hpp"
#include "svp/validation/embedded_svpi_validator.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <zip.h>

namespace {

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("embedded SVPI validation test requirement failed");
  }
}

void write_u32(std::ostream& output, std::uint32_t value) {
  const char bytes[] = {
      static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
      static_cast<char>(value >> 8U), static_cast<char>(value)};
  output.write(bytes, sizeof(bytes));
}

void write_test_mp4(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary);
  for (const char* type : {"ftyp", "moov", "mdat"}) {
    write_u32(output, 8);
    output.write(type, 4);
  }
}

void write_payload(const std::filesystem::path& path) {
  int error = 0;
  zip_t* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
  require(archive != nullptr);
  static constexpr char binding[] = R"({
    "bindings":[{
      "media_role":"primary_source",
      "identity":{"full_file_blake3":{
        "state":"present",
        "value":"blake3:0000000000000000000000000000000000000000000000000000000000000000"
      }}
    }]
  })";
  zip_source_t* source = zip_source_buffer(
      archive, binding, sizeof(binding) - 1, 0);
  require(source != nullptr);
  require(zip_file_add(archive, "media_binding.json", source,
                       ZIP_FL_OVERWRITE) >= 0);
  require(zip_close(archive) == 0);
}

bool has_code(const svp::validation::ValidationReport& report,
              std::string_view code) {
  return std::ranges::any_of(report.errors, [&](const auto& finding) {
    return finding.code == code;
  });
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("svp-embedded-validation-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);

  const auto clean = root / "clean.mp4";
  const auto payload = root / "payload.svpi";
  const auto embedded = root / "embedded.mp4";
  write_test_mp4(clean);
  write_payload(payload);

  svp::validation::EmbeddedSvpiValidatorOptions options;
  options.validation_codes_path =
      std::filesystem::path(SVP_SOURCE_DIR) /
      "spec/registries/validation-codes.json";

  auto report = svp::validation::validate_embedded_svpi_mp4(clean, options);
  require(has_code(report, svp::validation::kCodeMp4SvpiNotFound));

  require(svp::package::embed_svpi_in_mp4(clean, payload, embedded).success);
  report = svp::validation::validate_embedded_svpi_mp4(embedded, options);
  require(report.embedding_transport.status == "valid");
  require(has_code(report, svp::validation::kCodeMp4EmbeddedSvpiInvalid));
  require(has_code(report, svp::validation::kCodeMp4SvpiMediaBindingMismatch));

  const auto inspection = svp::package::inspect_embedded_svpi(embedded);
  std::fstream tamper(embedded, std::ios::binary | std::ios::in | std::ios::out);
  tamper.seekp(static_cast<std::streamoff>(
      inspection.embeddings.front().payload_offset));
  tamper.put('X');
  tamper.close();
  report = svp::validation::validate_embedded_svpi_mp4(embedded, options);
  require(has_code(report, svp::validation::kCodeMp4SvpiPayloadHashMismatch));

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return 0;
}
