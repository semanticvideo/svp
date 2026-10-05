#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct zip;
struct zip_file;

namespace package_export {

struct PackageEntry {
  std::string name;
  std::uint64_t zip_index = 0;
  std::uint64_t size_bytes = 0;
  bool is_directory = false;
};

// Where the package ZIP sits inside the input file. Absent for .svp and .svpi
// files; the verified embedded payload for an Embedded SVPI Transport.
struct PackageByteRange {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

// Read-only view of one package ZIP, opened once for the whole export. Entry
// contents are only ever streamed in bounded chunks, and every entry must
// yield exactly the uncompressed size its directory record declares.
class PackageReader {
 public:
  class EntryStream {
   public:
    EntryStream(EntryStream&& other) noexcept;
    EntryStream& operator=(EntryStream&&) = delete;
    EntryStream(const EntryStream&) = delete;
    EntryStream& operator=(const EntryStream&) = delete;
    ~EntryStream();

    // Reads up to `capacity` bytes. Returns 0 only at the declared end of the
    // entry. Throws ExportError when the entry is unreadable or its data is
    // shorter or longer than declared.
    [[nodiscard]] std::size_t read(char* buffer, std::size_t capacity);

    // Reads exactly `byte_count` bytes; reports failures through
    // `error_message` instead of throwing (block stream parser contract).
    [[nodiscard]] bool read_exact(std::byte* output, std::size_t byte_count,
                                  std::string& error_message);

    [[nodiscard]] std::uint64_t remaining() const noexcept {
      return remaining_;
    }

   private:
    friend class PackageReader;
    EntryStream(zip_file* file, std::string entry, std::uint64_t size);

    void verify_end();

    zip_file* file_ = nullptr;
    std::string entry_;
    std::uint64_t remaining_ = 0;
    bool end_verified_ = false;
  };

  PackageReader(const std::filesystem::path& path,
                std::optional<PackageByteRange> range);
  ~PackageReader();

  PackageReader(const PackageReader&) = delete;
  PackageReader& operator=(const PackageReader&) = delete;
  PackageReader(PackageReader&&) = delete;
  PackageReader& operator=(PackageReader&&) = delete;

  // Every entry in central-directory order, directories included.
  [[nodiscard]] const std::vector<PackageEntry>& entries() const noexcept {
    return entries_;
  }

  // First file entry with exactly this name, or nullptr.
  [[nodiscard]] const PackageEntry* find_file(std::string_view name) const;

  [[nodiscard]] EntryStream open(const PackageEntry& entry) const;

  // Whole entry in memory. Entries larger than `max_bytes` are refused with
  // record_too_large; callers pass the bound that owns that entry kind.
  [[nodiscard]] std::string read_whole(const PackageEntry& entry,
                                       std::uint64_t max_bytes) const;

 private:
  zip* archive_ = nullptr;
  std::vector<PackageEntry> entries_;
};

}  // namespace package_export
