#include "export_test_support.hpp"

#include <sys/wait.h>
#include <unistd.h>
#include <zip.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace export_test {
namespace {

void write_u32_be(std::ostream& output, std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
      static_cast<char>(value >> 8U), static_cast<char>(value)};
  output.write(bytes.data(), bytes.size());
}

}  // namespace

void check(bool condition, std::string_view expression, std::string_view file,
           int line) {
  if (!condition) {
    throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                             ": check failed: " + std::string{expression});
  }
}

TempDir::TempDir(std::string_view label) {
  static std::atomic<unsigned> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  path_ = std::filesystem::temp_directory_path() /
          ("svp-export-" + std::string{label} + "-" + std::to_string(::getpid()) +
           "-" + std::to_string(stamp) + "-" + std::to_string(counter++));
  std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

nlohmann::json CommandResult::json() const {
  return nlohmann::json::parse(stdout_text);
}

CommandResult run_command(const std::vector<std::string>& argv) {
  std::array<int, 2> pipe_ends{};
  if (::pipe(pipe_ends.data()) != 0) {
    throw std::runtime_error("pipe failed");
  }
  const pid_t child = ::fork();
  if (child < 0) {
    throw std::runtime_error("fork failed");
  }
  if (child == 0) {
    ::dup2(pipe_ends[1], STDOUT_FILENO);
    ::close(pipe_ends[0]);
    ::close(pipe_ends[1]);
    std::vector<char*> arguments;
    for (const auto& argument : argv) {
      arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    ::execv(arguments.front(), arguments.data());
    ::_exit(127);
  }
  ::close(pipe_ends[1]);
  CommandResult result;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = ::read(pipe_ends[0], buffer.data(), buffer.size());
    if (count <= 0) {
      break;
    }
    result.stdout_text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(pipe_ends[0]);
  int status = 0;
  if (::waitpid(child, &status, 0) < 0 || !WIFEXITED(status)) {
    throw std::runtime_error("child did not exit normally");
  }
  result.exit_code = WEXITSTATUS(status);
  return result;
}

CommandResult run_export(const std::filesystem::path& inspector,
                         const std::filesystem::path& package,
                         const std::filesystem::path& out, bool overwrite) {
  std::vector<std::string> argv{inspector.string(), "export", package.string(),
                                "--out", out.string()};
  if (overwrite) {
    argv.emplace_back("--overwrite");
  }
  return run_command(argv);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read " + path.string());
  }
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

std::map<std::string, std::string> read_tree(const std::filesystem::path& root) {
  std::map<std::string, std::string> files;
  for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
    if (item.is_regular_file()) {
      files.emplace(item.path().lexically_relative(root).generic_string(),
                    read_file(item.path()));
    }
  }
  return files;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::istringstream input(read_file(path));
  for (std::string line; std::getline(input, line);) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
  std::vector<nlohmann::json> records;
  for (const auto& line : read_lines(path)) {
    records.push_back(nlohmann::json::parse(line));
  }
  return records;
}

void write_zip(const std::filesystem::path& path,
               const std::vector<ZipEntrySpec>& entries) {
  int error = 0;
  zip_t* archive =
      zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
  if (archive == nullptr) {
    throw std::runtime_error("zip_open failed for " + path.string());
  }
  for (const auto& entry : entries) {
    zip_source_t* source = zip_source_buffer(
        archive, entry.content.data(), entry.content.size(), 0);
    if (source == nullptr) {
      throw std::runtime_error("zip_source_buffer failed");
    }
    const auto index =
        zip_file_add(archive, entry.name.c_str(), source, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
      zip_source_free(source);
      throw std::runtime_error("zip_file_add failed for " + entry.name + ": " +
                               zip_strerror(archive));
    }
    if (entry.store &&
        zip_set_file_compression(archive, static_cast<zip_uint64_t>(index),
                                 ZIP_CM_STORE, 0) != 0) {
      throw std::runtime_error("zip_set_file_compression failed");
    }
  }
  if (zip_close(archive) != 0) {
    const std::string message = zip_strerror(archive);
    zip_discard(archive);
    throw std::runtime_error("zip_close failed: " + message);
  }
}

std::vector<std::string> zip_file_entries(const std::filesystem::path& path) {
  int error = 0;
  zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
  if (archive == nullptr) {
    throw std::runtime_error("zip_open failed for " + path.string());
  }
  std::vector<std::string> names;
  const auto count = zip_get_num_entries(archive, 0);
  for (zip_int64_t index = 0; index < count; ++index) {
    const char* name = zip_get_name(archive, static_cast<zip_uint64_t>(index), 0);
    if (name != nullptr && std::string_view{name}.back() != '/') {
      names.emplace_back(name);
    }
  }
  zip_discard(archive);
  return names;
}

std::string zip_entry_content(const std::filesystem::path& path,
                              const std::string& name) {
  int error = 0;
  zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
  if (archive == nullptr) {
    throw std::runtime_error("zip_open failed for " + path.string());
  }
  zip_stat_t stat;
  zip_stat_init(&stat);
  std::string content;
  if (zip_stat(archive, name.c_str(), 0, &stat) == 0) {
    zip_file_t* file = zip_fopen(archive, name.c_str(), 0);
    content.resize(static_cast<std::size_t>(stat.size));
    const auto count = file == nullptr
                           ? -1
                           : zip_fread(file, content.data(), content.size());
    if (file != nullptr) {
      zip_fclose(file);
    }
    if (count != static_cast<zip_int64_t>(content.size())) {
      zip_discard(archive);
      throw std::runtime_error("cannot read " + name);
    }
  } else {
    zip_discard(archive);
    throw std::runtime_error("no entry " + name);
  }
  zip_discard(archive);
  return content;
}

bool has_export_leftovers(const std::filesystem::path& parent) {
  for (const auto& item : std::filesystem::directory_iterator(parent)) {
    const auto name = item.path().filename().string();
    if (name.find(".svp-export-") != std::string::npos) {
      return true;
    }
  }
  return false;
}

void write_iso_bmff_container(const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary);
  write_u32_be(output, 20);
  output.write("ftyp", 4);
  output.write("isom", 4);
  write_u32_be(output, 0);
  output.write("isom", 4);
  for (const char* type : {"moov", "mdat"}) {
    write_u32_be(output, 8);
    output.write(type, 4);
  }
}

}  // namespace export_test
