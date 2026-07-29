#pragma once

#include "svp/progress/renderer.hpp"

#include <filesystem>
#include <ostream>

namespace svp::models::tool {

int install_reference_models(const std::filesystem::path& executable,
                             const std::filesystem::path& cache_dir,
                             int parallel_downloads,
                             svp::progress::Mode progress_mode,
                             std::ostream& progress_stream,
                             bool is_tty,
                             int terminal_fd);

}  // namespace svp::models::tool
