#include "embedded_input_controller.hpp"

#include "svp/package/embedded_svpi.hpp"
#include "svp/package/package_layout.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>

namespace {

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"          \
                << __LINE__ << "\n";                                         \
      std::abort();                                                          \
    }                                                                        \
  } while (0)

void write_u32_be(std::ostream& output, std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value >> 24U),
      static_cast<char>(value >> 16U),
      static_cast<char>(value >> 8U),
      static_cast<char>(value),
  };
  output.write(bytes.data(), bytes.size());
}

void write_test_container(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary);
  write_u32_be(output, 24);
  output.write("ftyp", 4);
  output.write("mp42", 4);
  write_u32_be(output, 0);
  output.write("mp42", 4);
  output.write("isom", 4);
  write_u32_be(output, 8);
  output.write("mdat", 4);
}

}  // namespace

int main() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
      ("svp-inspector-embedded-input-" + std::to_string(suffix));
  std::filesystem::create_directories(root);
  const auto original = root / "original.mp4";
  const auto svpi = root / "package.svpi";
  const auto embedded = root / "embedded.mp4";
  write_test_container(original);
  {
    std::ofstream payload(svpi, std::ios::binary);
    payload.write("PK\x03\x04transport", 13);
  }
  CHECK(svp::package::embed_svpi_in_iso_bmff(
            original, svpi, embedded).success);

  {
    const auto decision = embedded_input_controller::preflight(embedded);
    CHECK(decision.handled_as_transport);
    CHECK(decision.semantic_access_allowed);
    const auto payload_offset =
        decision.inspection().embeddings.front().payload_offset;
    std::fstream file(embedded,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(payload_offset));
    file.put('\0');
    file.close();
    const auto layout = svp::package::read_package_layout(embedded);
    CHECK(!layout.has_value());
    CHECK(layout.error_message().find("BLAKE3") != std::string::npos);
  }
  const auto tampered_decision =
      embedded_input_controller::preflight(embedded);
  CHECK(tampered_decision.handled_as_transport);
  CHECK(!tampered_decision.semantic_access_allowed);
  CHECK(std::ranges::any_of(
      tampered_decision.inspection().issues, [](const auto& issue) {
        return issue.code ==
            svp::package::EmbeddedSvpiIssueCode::payload_hash_mismatch;
      }));

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return 0;
}
