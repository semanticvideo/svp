#include "svp/package/package_writer.hpp"

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
  return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool should_store_uncompressed(std::string_view name) {
  if (name == "mimetype") {
    return true;
  }
  if (name == "index/index.sqlite") {
    return true;
  }
  if (starts_with(name, "spatial/") && (ends_with(name, ".svpdz") || ends_with(name, ".svpmz"))) {
    return true;
  }
  if (starts_with(name, "embeddings/") && ends_with(name, ".svpez")) {
    return true;
  }
  if (starts_with(name, "media/original/")) {
    return true;
  }
  return false;
}

void add_ancestors(const std::string& entry_path, std::set<std::string>& dirs) {
  std::size_t slash = entry_path.find('/');
  while (slash != std::string::npos) {
    dirs.insert(entry_path.substr(0, slash + 1));
    slash = entry_path.find('/', slash + 1);
  }
}

}  // namespace

bool write_package_skeleton(
    const std::filesystem::path& package_path,
    const std::filesystem::path& staging_dir,
    const std::filesystem::path& source_path,
    const nlohmann::json& manifest_json,
    const PackageWriterOptions& options) {
  try {
    std::filesystem::create_directories(package_path.parent_path());
    std::filesystem::path temp_path = package_path.string() + ".tmp";

    // 1. Prepare contents that need to be kept alive for the buffer sources
    std::string mimetype_content = "application/vnd.svp+zip";
    std::string manifest_content = manifest_json.dump(2) + "\n";

    // 2. Open ZIP
    int error_code = ZIP_ER_OK;
    zip_t* archive = zip_open(temp_path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error_code);
    if (!archive) {
      throw std::runtime_error("failed to open zip file for writing: " + zip_error_message(error_code));
    }

    // Use a custom deleter to ensure clean discard/close and atomic cleanup
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

    // 3. Add mimetype first
    zip_source_t* mime_source = zip_source_buffer(archive, mimetype_content.data(), mimetype_content.size(), 0);
    if (!mime_source) {
      throw std::runtime_error("failed to create zip source for mimetype");
    }
    zip_int64_t mime_index = zip_file_add(archive, "mimetype", mime_source, ZIP_FL_OVERWRITE);
    if (mime_index < 0) {
      zip_source_free(mime_source);
      throw std::runtime_error("failed to add mimetype to zip");
    }
    if (zip_set_file_compression(archive, mime_index, ZIP_CM_STORE, 0) < 0) {
      throw std::runtime_error("failed to set compression for mimetype");
    }

    // 4. Gather other entries
    std::set<std::string> dirs_to_add;
    std::map<std::string, std::filesystem::path> files_to_add;

    // Add required directories
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
      "provenance/"
    };
    for (const auto& d : required_dirs) {
      dirs_to_add.insert(d);
    }

    // Walk staging directory
    if (std::filesystem::exists(staging_dir) && std::filesystem::is_directory(staging_dir)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(staging_dir)) {
        std::filesystem::path rel_path = std::filesystem::relative(entry.path(), staging_dir);
        std::string rel_str = rel_path.string();
        // Normalize slashes
        std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

        if (entry.is_directory()) {
          dirs_to_add.insert(rel_str + "/");
        } else if (entry.is_regular_file()) {
          files_to_add[rel_str] = entry.path();
          add_ancestors(rel_str, dirs_to_add);
        }
      }
    }

    // Add source media file
    if (!source_path.empty() && std::filesystem::exists(source_path)) {
      std::string ext = source_path.extension().string();
      std::string dest_path = "media/original/source_000" + ext;
      files_to_add[dest_path] = source_path;
      add_ancestors(dest_path, dirs_to_add);
    }

    // Create operations list
    struct ZipEntryOp {
      std::string name;
      bool is_directory;
      std::filesystem::path source_path;
      bool is_manifest = false;
    };
    std::vector<ZipEntryOp> ops;

    for (const auto& d : dirs_to_add) {
      ops.push_back(ZipEntryOp{d, true, {}});
    }
    for (const auto& pair : files_to_add) {
      ops.push_back(ZipEntryOp{pair.first, false, pair.second});
    }
    ops.push_back(ZipEntryOp{"manifest.json", false, {}, true});

    // Sort alphabetically for deterministic ordering
    if (options.deterministic_ordering) {
      std::sort(ops.begin(), ops.end(), [](const ZipEntryOp& a, const ZipEntryOp& b) {
        return a.name < b.name;
      });
    }

    // 5. Add all sorted entries
    for (const auto& op : ops) {
      if (op.is_directory) {
        zip_dir_add(archive, op.name.c_str(), 0);
      } else if (op.is_manifest) {
        zip_source_t* manifest_source = zip_source_buffer(archive, manifest_content.data(), manifest_content.size(), 0);
        if (!manifest_source) {
          throw std::runtime_error("failed to create zip source for manifest.json");
        }
        zip_int64_t idx = zip_file_add(archive, "manifest.json", manifest_source, ZIP_FL_OVERWRITE);
        if (idx < 0) {
          zip_source_free(manifest_source);
          throw std::runtime_error("failed to add manifest.json to zip");
        }
      } else {
        zip_source_t* file_source = zip_source_file(archive, op.source_path.string().c_str(), 0, 0);
        if (!file_source) {
          throw std::runtime_error("failed to create zip source for staged file: " + op.source_path.string());
        }
        zip_int64_t idx = zip_file_add(archive, op.name.c_str(), file_source, ZIP_FL_OVERWRITE);
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

    // 6. Close zip successfully
    if (zip_close(archive) < 0) {
      throw std::runtime_error("failed to close zip file");
    }
    guard.archive = nullptr;

    // 7. Atomic rename
    std::filesystem::rename(temp_path, package_path);
    guard.success = true;
    return true;

  } catch (const std::exception& error) {
    std::cerr << "write_package_skeleton error: " << error.what() << "\n";
    return false;
  }
}

}  // namespace svp::package
