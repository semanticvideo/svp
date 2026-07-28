#pragma once

#include <filesystem>
#include <string_view>

namespace dump_command {

int run(const std::filesystem::path& package_path, std::string_view section);

}  // namespace dump_command
