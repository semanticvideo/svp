#pragma once

// Helpers for the CAS and recovery-journal tests: scratch directories, file
// fixtures, and typed-error assertions.

#include "exec_test_support.hpp"
#include "svp/exec/cache_error.hpp"
#include "svp/exec/journal_error.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace svp::exec::test {

namespace fs = std::filesystem;

struct TemporaryDirectory {
  fs::path path;

  explicit TemporaryDirectory(std::string_view suite) {
    static std::atomic<int> counter{0};
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           (std::string(suite) + "-" + std::to_string(::getpid()) + "-" +
            std::to_string(nonce) + "-" + std::to_string(counter.fetch_add(1)));
    fs::create_directories(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    // Published blobs are read-only; make everything writable first.
    for (auto entry = fs::recursive_directory_iterator(path, ignored);
         entry != fs::recursive_directory_iterator(); entry.increment(ignored)) {
      fs::permissions(entry->path(), fs::perms::owner_all, fs::perm_options::add, ignored);
    }
    fs::remove_all(path, ignored);
  }
};

inline void write_file(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::error_code ignored;
  fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ignored);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!output) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

inline std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::size_t count_entries(const fs::path& directory) {
  std::size_t count = 0;
  for ([[maybe_unused]] const auto& entry : fs::directory_iterator(directory)) {
    ++count;
  }
  return count;
}

template <typename T>
void expect_cache_error(const CacheResult<T>& result, CacheErrorCode expected,
                        std::string_view message) {
  if (result.ok()) {
    throw std::runtime_error(std::string(message) + ": expected " +
                             std::string(cache_error_code_name(expected)) + " but succeeded");
  }
  if (result.error().code != expected) {
    throw std::runtime_error(std::string(message) + ": expected " +
                             std::string(cache_error_code_name(expected)) + " but got " +
                             std::string(cache_error_code_name(result.error().code)) + " (" +
                             result.error().message + ")");
  }
}

template <typename T>
T expect_ok(CacheResult<T> result, std::string_view message) {
  if (!result.ok()) {
    throw std::runtime_error(std::string(message) + ": " +
                             std::string(cache_error_code_name(result.error().code)) + " (" +
                             result.error().message + ")");
  }
  return std::move(result).value();
}

template <typename Function>
void expect_journal_error(JournalErrorCode expected, Function&& function,
                          std::string_view message) {
  try {
    function();
  } catch (const JournalError& error) {
    if (error.code() != expected) {
      throw std::runtime_error(std::string(message) + ": expected " +
                               std::string(journal_error_code_name(expected)) + " but got " +
                               std::string(journal_error_code_name(error.code())) + " (" +
                               error.what() + ")");
    }
    return;
  }
  throw std::runtime_error(std::string(message) + ": no JournalError was thrown");
}

inline std::span<const std::byte> as_byte_span(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace svp::exec::test
