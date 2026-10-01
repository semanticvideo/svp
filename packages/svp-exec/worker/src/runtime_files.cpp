#include "runtime_files.hpp"

#include "svp/exec/worker/worker_error.hpp"

#include <fstream>
#include <string>
#include <system_error>

namespace svp::exec::worker::detail {

std::filesystem::perms runtime_file_mode(std::string_view relative_path) {
  using std::filesystem::perms;
  const std::filesystem::path path{std::string(relative_path)};
  const bool in_bin = path.has_parent_path() && path.parent_path().filename() == "bin";
  return in_bin ? perms::owner_all | perms::group_read | perms::group_exec |
                      perms::others_read | perms::others_exec
                : perms::owner_read | perms::owner_write | perms::group_read |
                      perms::others_read;
}

void write_text_file(const std::filesystem::path& path, std::string_view bytes) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.close();
  if (!stream) {
    throw WorkerError(WorkerErrorCode::io, "cannot write " + path.string());
  }
}

}  // namespace svp::exec::worker::detail
