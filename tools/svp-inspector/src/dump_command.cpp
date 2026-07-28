#include "dump_command.hpp"

#include "svp/package/package_layout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace dump_command {
namespace {

struct DumpTarget {
  std::string_view section;
  std::string_view entry;
};

const std::vector<DumpTarget>& dump_targets() {
  static const std::vector<DumpTarget> targets{
      {"manifest", "manifest.json"},
      {"index_manifest", "index/index_manifest.json"},
  };
  return targets;
}

const DumpTarget* find_dump_target(std::string_view section) {
  for (const auto& target : dump_targets()) {
    if (target.section == section) {
      return &target;
    }
  }
  return nullptr;
}

int dump_json_entry(const std::filesystem::path& package_path,
                    const DumpTarget& target) {
  const auto read_result = svp::package::read_package_entry(
      package_path, std::string{target.entry});
  if (!read_result.has_value()) {
    std::cerr << "Unable to read " << target.entry << ": "
              << read_result.error_message() << "\n";
    return 1;
  }

  const auto parsed =
      nlohmann::json::parse(read_result.value(), nullptr, false);
  if (parsed.is_discarded()) {
    std::cerr << "Unable to parse " << target.entry << " as JSON\n";
    return 1;
  }

  std::cout << parsed.dump(2) << "\n";
  return 0;
}

}  // namespace

int run(const std::filesystem::path& package_path, std::string_view section) {
  if (section == "all") {
    int status = 0;
    bool first = true;
    for (const auto& target : dump_targets()) {
      if (!first) {
        std::cout << "\n";
      }
      first = false;
      std::cout << "# " << target.entry << "\n";
      status = std::max(status, dump_json_entry(package_path, target));
    }
    return status;
  }

  const auto* target = find_dump_target(section);
  if (target == nullptr) {
    std::cerr << "Unsupported dump section: " << section
              << " (expected manifest, index_manifest, or all)\n";
    return 2;
  }

  return dump_json_entry(package_path, *target);
}

}  // namespace dump_command
