#include "svp/package/svpi_writer.hpp"
#include "svp/package/svpi_media_policy.hpp"

#include <zip.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace svp::package {
namespace {

std::string zip_error_message(int error_code) {
  zip_error_t error;
  zip_error_init_with_code(&error, error_code);
  std::string message = zip_error_strerror(&error);
  zip_error_fini(&error);
  return message;
}

bool starts_with(std::string_view str, std::string_view prefix) {
  return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view str, std::string_view suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool should_store_uncompressed(std::string_view name) {
  if (name == "mimetype") {
    return true;
  }
  if (name == "index/index.sqlite") {
    return true;
  }
  if (starts_with(name, "spatial/") &&
      (ends_with(name, ".svpdz") || ends_with(name, ".svpmz"))) {
    return true;
  }
  if (starts_with(name, "embeddings/") && ends_with(name, ".svpez")) {
    return true;
  }
  return false;
}

void add_ancestors(const std::string& entry_path, std::set<std::string>& dirs) {
  std::size_t slash = entry_path.find('/');
  while (slash != std::string::npos) {
    std::string prefix = entry_path.substr(0, slash + 1);
    if (is_svpi_entry_forbidden(prefix)) {
      return;
    }
    dirs.insert(prefix);
    slash = entry_path.find('/', slash + 1);
  }
}

}  // namespace

bool write_svpi_package(
    const std::filesystem::path& package_path,
    const std::filesystem::path& staging_dir,
    const nlohmann::json& manifest_json,
    const MediaBindingDocument& media_binding,
    const SvpiWriterOptions& options) {
  try {
    std::filesystem::create_directories(package_path.parent_path());
    std::filesystem::path temp_path = package_path.string() + ".tmp";

    std::string mimetype_content(std::string{kSvpiMimetype});
    std::string manifest_content = manifest_json.dump(2) + "\n";
    std::string binding_content = to_json(media_binding).dump(2) + "\n";

    int error_code = ZIP_ER_OK;
    zip_t* archive =
        zip_open(temp_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error_code);
    if (!archive) {
      throw std::runtime_error("failed to open zip file for writing: " +
                               zip_error_message(error_code));
    }

    struct ZipGuard {
      zip_t* archive;
      std::filesystem::path temp_path;
      bool success = false;
      ~ZipGuard() {
        if (archive) {
          zip_discard(archive);
        }
        if (!success) {
          if (std::filesystem::exists(temp_path)) {
            std::filesystem::remove(temp_path);
          }
        }
      }
    } guard{archive, temp_path};

    // 1. Add mimetype first (uncompressed)
    {
      zip_source_t* source =
          zip_source_buffer(archive, mimetype_content.data(), mimetype_content.size(), 0);
      if (!source) {
        throw std::runtime_error("failed to create zip source for mimetype");
      }
      zip_int64_t idx = zip_file_add(archive, "mimetype", source, ZIP_FL_OVERWRITE);
      if (idx < 0) {
        zip_source_free(source);
        throw std::runtime_error("failed to add mimetype to zip");
      }
      if (zip_set_file_compression(archive, idx, ZIP_CM_STORE, 0) < 0) {
        throw std::runtime_error("failed to set compression for mimetype");
      }
    }

    // 2. Verify required spine files exist in staging
    const std::vector<std::string> required_spine_files = {
        "provenance/processors.jsonl",
        "provenance/interlace_events.jsonl",
        "index/index.sqlite",
        "index/index_manifest.json",
    };
    for (const auto& rel : required_spine_files) {
      const auto full_path = staging_dir / rel;
      if (!std::filesystem::exists(full_path)) {
        throw std::runtime_error("required spine file missing from staging: " + rel);
      }
    }

    // 3. Gather entries from staging directory
    std::set<std::string> dirs_to_add;
    std::map<std::string, std::filesystem::path> files_to_add;

    // Required directories (same as SVP, but media/ is still needed for
    // evidence crops — just not media/original/)
    std::vector<std::string> required_dirs = {
        "media/",
        "transcript/",
        "timeline/",
        "entities/",
        "spatial/",
        "text/",
        "colors/",
        "relationships/",
        "embeddings/",
        "index/",
        "provenance/",
    };
    for (const auto& d : required_dirs) {
      dirs_to_add.insert(d);
    }

    // Walk staging directory, excluding media/original/
    if (std::filesystem::exists(staging_dir) &&
        std::filesystem::is_directory(staging_dir)) {
      for (const auto& entry :
           std::filesystem::recursive_directory_iterator(staging_dir)) {
        std::filesystem::path rel_path =
            std::filesystem::relative(entry.path(), staging_dir);
        std::string rel_str = rel_path.string();
        std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

        // Skip forbidden entries — media/original/ and replayable audio derivatives
        if (is_svpi_entry_forbidden(rel_str)) {
          continue;
        }

        if (entry.is_directory()) {
          dirs_to_add.insert(rel_str + "/");
        } else if (entry.is_regular_file()) {
          files_to_add[rel_str] = entry.path();
          add_ancestors(rel_str, dirs_to_add);
        }
      }
    }

    // Build operations list
    struct ZipEntryOp {
      std::string name;
      bool is_directory;
      std::filesystem::path source_path;
      enum Kind { staged_file, manifest, media_binding } kind = staged_file;
    };
    std::vector<ZipEntryOp> ops;

    for (const auto& d : dirs_to_add) {
      ops.push_back(ZipEntryOp{d, true, {}});
    }
    for (const auto& pair : files_to_add) {
      ops.push_back(ZipEntryOp{pair.first, false, pair.second});
    }
    ops.push_back(ZipEntryOp{"manifest.json", false, {}, ZipEntryOp::manifest});
    ops.push_back(
        ZipEntryOp{"media_binding.json", false, {}, ZipEntryOp::media_binding});

    // Sort alphabetically for deterministic ordering
    if (options.deterministic_ordering) {
      std::sort(ops.begin(), ops.end(),
                [](const ZipEntryOp& a, const ZipEntryOp& b) { return a.name < b.name; });
    }

    // 3. Add all entries
    for (const auto& op : ops) {
      if (op.is_directory) {
        zip_dir_add(archive, op.name.c_str(), 0);
      } else if (op.kind == ZipEntryOp::manifest) {
        zip_source_t* source = zip_source_buffer(archive, manifest_content.data(),
                                                  manifest_content.size(), 0);
        if (!source) {
          throw std::runtime_error("failed to create zip source for manifest.json");
        }
        zip_int64_t idx =
            zip_file_add(archive, "manifest.json", source, ZIP_FL_OVERWRITE);
        if (idx < 0) {
          zip_source_free(source);
          throw std::runtime_error("failed to add manifest.json to zip");
        }
      } else if (op.kind == ZipEntryOp::media_binding) {
        zip_source_t* source = zip_source_buffer(archive, binding_content.data(),
                                                  binding_content.size(), 0);
        if (!source) {
          throw std::runtime_error("failed to create zip source for media_binding.json");
        }
        zip_int64_t idx = zip_file_add(archive, "media_binding.json", source,
                                        ZIP_FL_OVERWRITE);
        if (idx < 0) {
          zip_source_free(source);
          throw std::runtime_error("failed to add media_binding.json to zip");
        }
      } else {
        zip_source_t* file_source =
            zip_source_file(archive, op.source_path.string().c_str(), 0, 0);
        if (!file_source) {
          throw std::runtime_error("failed to create zip source for staged file: " +
                                   op.source_path.string());
        }
        zip_int64_t idx =
            zip_file_add(archive, op.name.c_str(), file_source, ZIP_FL_OVERWRITE);
        if (idx < 0) {
          zip_source_free(file_source);
          throw std::runtime_error("failed to add file " + op.name + " to zip");
        }
        if (should_store_uncompressed(op.name)) {
          if (zip_set_file_compression(archive, idx, ZIP_CM_STORE, 0) < 0) {
            throw std::runtime_error("failed to set compression for " + op.name);
          }
        }
      }
    }

    // 4. Close zip successfully
    if (zip_close(archive) < 0) {
      throw std::runtime_error("failed to close zip file");
    }
    guard.archive = nullptr;

    // 5. Atomic rename
    std::filesystem::rename(temp_path, package_path);
    guard.success = true;
    return true;

  } catch (const std::exception& error) {
    std::cerr << "write_svpi_package error: " << error.what() << "\n";
    return false;
  }
}

}  // namespace svp::package
