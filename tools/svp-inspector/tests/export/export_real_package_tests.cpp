// Exports real packages produced by a full build, when the caller provides
// them. Real packages are large and are never committed.
//
// Usage: svp-inspector-export-real-package-tests <svp-inspector>
// Environment: SVP_EXPORT_REAL_PACKAGES_DIR names a directory whose .svp and
// .svpi files, and ISO BMFF files (.mp4, .mov, .m4v) carrying an Embedded SVPI
// Transport, are exported. Without it the test reports itself skipped.

#include "export_test_support.hpp"

#include "svp/package/embedded_svpi.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using export_test::TempDir;
using nlohmann::json;
namespace fs = std::filesystem;

std::vector<fs::path> real_packages() {
  const char* directory = std::getenv("SVP_EXPORT_REAL_PACKAGES_DIR");
  std::vector<fs::path> packages;
  if (directory == nullptr || std::string{directory}.empty() ||
      !fs::is_directory(directory)) {
    return packages;
  }
  for (const auto& item : fs::directory_iterator(directory)) {
    const auto extension = item.path().extension();
    if (item.is_regular_file() &&
        (extension == ".svp" || extension == ".svpi" || extension == ".mp4" ||
         extension == ".mov" || extension == ".m4v")) {
      packages.push_back(item.path());
    }
  }
  std::ranges::sort(packages);
  return packages;
}

std::uint64_t count_records(const std::string& bytes) {
  std::uint64_t count = 0;
  std::size_t start = 0;
  while (start < bytes.size()) {
    auto end = bytes.find('\n', start);
    if (end == std::string::npos) {
      end = bytes.size();
    }
    const auto line = std::string_view{bytes}.substr(start, end - start);
    if (line.find_first_not_of(" \t\r") != std::string_view::npos) {
      ++count;
    }
    start = end + 1;
  }
  return count;
}

bool is_zip_package(const fs::path& path) {
  return path.extension() == ".svp" || path.extension() == ".svpi";
}

void check_package(const fs::path& inspector, const fs::path& input) {
  TempDir temp{"real"};
  // For an embedded transport, the package is the embedded SVPI.
  auto package = input;
  if (!is_zip_package(input)) {
    package = temp.path() / "embedded.svpi";
    EXPORT_CHECK(svp::package::extract_embedded_svpi(input, package).success);
  }
  const auto out = temp.path() / "export";
  const auto result = export_test::run_export(inspector, input, out);
  if (result.exit_code != 0) {
    std::cerr << result.stdout_text << "\n";
  }
  EXPORT_CHECK(result.exit_code == 0);
  const auto summary = json::parse(export_test::read_file(out / "export.json"));
  EXPORT_CHECK(summary.at("package").at("kind") ==
               (input.extension() == ".svp"    ? "svp"
                : input.extension() == ".svpi" ? "svpi"
                                               : "embedded_svpi"));

  // Every file entry is exported as a layer, and every listed file exists.
  auto entries = export_test::zip_file_entries(package);
  std::ranges::sort(entries);
  std::vector<std::string> layers;
  for (const auto& layer : summary.at("layers")) {
    layers.push_back(layer.at("entry").get<std::string>());
    for (const auto& file : layer.at("files")) {
      const auto path = out / file.at("path").get<std::string>();
      EXPORT_CHECK(fs::is_regular_file(path));
      EXPORT_CHECK(fs::file_size(path) ==
                   file.at("size_bytes").get<std::uint64_t>());
    }
    const auto& entry = layer.at("entry").get_ref<const std::string&>();
    const auto representation = layer.at("representation").get<std::string>();
    if (representation == "jsonl") {
      // One exported record per package record.
      EXPORT_CHECK(layer.at("record_count").get<std::uint64_t>() ==
                   count_records(export_test::zip_entry_content(package, entry)));
    } else if (representation == "json" || representation == "file") {
      EXPORT_CHECK(export_test::read_file(out / "layers" / entry) ==
                   export_test::zip_entry_content(package, entry));
    }
  }
  EXPORT_CHECK(layers == entries);

  // Text observations carry their resolved sample times.
  const auto observations = out / "layers" / "text" / "text_observations.jsonl";
  if (fs::exists(observations)) {
    for (const auto& record : export_test::read_jsonl(observations)) {
      if (!record.contains("source_frame_ids")) {
        continue;
      }
      const auto& resolved = record.at("svp_export").at("source_frame_ids");
      EXPORT_CHECK(resolved.size() == record.at("source_frame_ids").size());
      for (const auto& frame : resolved) {
        EXPORT_CHECK(frame.at("found") == true);
        EXPORT_CHECK(frame.at("pts_us").is_number_integer());
      }
    }
  }

  // The same package always exports to the same bytes.
  const auto again = temp.path() / "again";
  EXPORT_CHECK(export_test::run_export(inspector, input, again).exit_code == 0);
  EXPORT_CHECK(export_test::read_tree(out) == export_test::read_tree(again));
  std::cout << input.filename().string() << " ("
            << summary.at("package").at("kind").get<std::string>() << "): "
            << layers.size() << " layers exported\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: svp-inspector-export-real-package-tests <svp-inspector>\n";
    return 2;
  }
  const auto packages = real_packages();
  if (packages.empty()) {
    std::cout << "SVP_EXPORT_REAL_PACKAGES_DIR names no packages; skipping\n";
    return export_test::kSkipExitCode;
  }
  try {
    for (const auto& package : packages) {
      check_package(argv[1], package);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
  return 0;
}
