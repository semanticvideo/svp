#include "svp/package/index_writer.hpp"
#include "svp/package/index_logical_rows.hpp"
#include "svp/package/relationship_type_policy.hpp"
#include "svp/core/memory_diagnostics.hpp"

#include <blake3.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace svp::package {
namespace {

struct SqliteDeleter {
  void operator()(sqlite3* database) const noexcept {
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }
};

struct StatementDeleter {
  void operator()(sqlite3_stmt* statement) const noexcept {
    if (statement != nullptr) {
      sqlite3_finalize(statement);
    }
  }
};

const std::vector<std::string> kIndexTables = {
    "CREATE TABLE svp_meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ")",

    "CREATE TABLE objects ("
    "  object_id TEXT PRIMARY KEY,"
    "  object_type TEXT NOT NULL,"
    "  start_us INTEGER,"
    "  end_us INTEGER,"
    "  json_path TEXT NOT NULL"
    ")",

    "CREATE TABLE temporal_spans ("
    "  object_id TEXT NOT NULL,"
    "  object_type TEXT NOT NULL,"
    "  start_us INTEGER NOT NULL,"
    "  end_us INTEGER NOT NULL"
    ")",

    "CREATE TABLE relationships ("
    "  relationship_id TEXT PRIMARY KEY,"
    "  relationship_type TEXT NOT NULL,"
    "  relationship_class TEXT NOT NULL,"
    "  source_id TEXT NOT NULL,"
    "  target_id TEXT NOT NULL,"
    "  start_us INTEGER NOT NULL,"
    "  end_us INTEGER NOT NULL,"
    "  confidence REAL NOT NULL"
    ")",

    "CREATE TABLE text_fts ("
    "  object_id TEXT,"
    "  object_type TEXT,"
    "  start_us INTEGER,"
    "  end_us INTEGER,"
    "  text TEXT"
    ")",

    "CREATE TABLE vector_index ("
    "  embedding BLOB,"
    "  object_id TEXT,"
    "  object_type TEXT,"
    "  embedding_set_id TEXT,"
    "  start_us INTEGER,"
    "  end_us INTEGER"
    ")",

    "CREATE TABLE binary_blocks ("
    "  block_id TEXT PRIMARY KEY,"
    "  block_type TEXT NOT NULL,"
    "  block_file TEXT NOT NULL,"
    "  block_offset INTEGER NOT NULL,"
    "  block_length INTEGER NOT NULL,"
    "  payload_offset INTEGER NOT NULL,"
    "  uncompressed_size INTEGER NOT NULL,"
    "  compressed_size INTEGER NOT NULL,"
    "  extent_0 INTEGER NOT NULL,"
    "  extent_1 INTEGER NOT NULL,"
    "  extent_2 INTEGER NOT NULL,"
    "  dtype INTEGER NOT NULL,"
    "  start_frame INTEGER,"
    "  frame_count INTEGER,"
    "  start_us INTEGER,"
    "  end_us INTEGER,"
    "  payload_blake3 TEXT NOT NULL,"
    "  header_blake3 TEXT NOT NULL,"
    "  block_blake3 TEXT NOT NULL"
    ")",

    "CREATE TABLE text_regions ("
    "  text_region_id TEXT PRIMARY KEY,"
    "  start_us INTEGER,"
    "  end_us INTEGER,"
    "  shot_id TEXT,"
    "  scene_id TEXT"
    ")",

    "CREATE TABLE text_observations ("
    "  text_observation_id TEXT PRIMARY KEY,"
    "  text_region_id TEXT NOT NULL,"
    "  raw_text TEXT NOT NULL,"
    "  normalized_text TEXT NOT NULL,"
    "  layout_class TEXT"
    ")",

    "CREATE TABLE numeric_values ("
    "  numeric_value_id TEXT PRIMARY KEY,"
    "  text_observation_id TEXT NOT NULL,"
    "  text_region_id TEXT NOT NULL,"
    "  numeric_value TEXT NOT NULL,"
    "  raw_text TEXT NOT NULL,"
    "  normalized_text TEXT NOT NULL"
    ")",

    "CREATE TABLE color_observations ("
    "  color_observation_id TEXT PRIMARY KEY,"
    "  target_type TEXT NOT NULL,"
    "  target_id TEXT NOT NULL,"
    "  dominant_bucket TEXT NOT NULL"
    ")",

    "CREATE TABLE color_bucket_coverage ("
    "  color_observation_id TEXT NOT NULL,"
    "  bucket_id TEXT NOT NULL,"
    "  coverage REAL NOT NULL"
    ")",

    "CREATE TABLE color_targets ("
    "  color_observation_id TEXT NOT NULL,"
    "  target_type TEXT NOT NULL,"
    "  target_id TEXT NOT NULL"
    ")",

    "CREATE TABLE entities ("
    "  entity_id TEXT PRIMARY KEY,"
    "  entity_type TEXT NOT NULL,"
    "  first_seen_us INTEGER,"
    "  last_seen_us INTEGER"
    ")",

    "CREATE TABLE entity_tracks ("
    "  track_id TEXT PRIMARY KEY,"
    "  entity_id TEXT NOT NULL,"
    "  start_us INTEGER,"
    "  end_us INTEGER,"
    "  confidence REAL"
    ")",

    "CREATE TABLE spatial_regions ("
    "  region_id TEXT PRIMARY KEY,"
    "  entity_id TEXT,"
    "  track_id TEXT,"
    "  frame_id TEXT,"
    "  pts_us INTEGER,"
    "  screen_area_ratio REAL,"
    "  confidence REAL"
    ")",

    "CREATE TABLE spatial_masks ("
    "  mask_id TEXT PRIMARY KEY,"
    "  entity_id TEXT,"
    "  track_id TEXT,"
    "  region_id TEXT,"
    "  frame_id TEXT,"
    "  width INTEGER,"
    "  height INTEGER"
    ")"
};

std::vector<nlohmann::json> read_jsonl(const std::filesystem::path& path) {
  svp::core::check_memory_limit("index.read_jsonl.begin", {
      {"path", path.string()},
      {"exists", std::filesystem::exists(path) ? "true" : "false"}
  });
  std::vector<nlohmann::json> records;
  if (!std::filesystem::exists(path)) {
    svp::core::check_memory_limit("index.read_jsonl.end", {
        {"path", path.string()},
        {"records", "0"},
        {"missing", "true"}
    });
    return records;
  }
  std::ifstream file(path);
  if (!file) {
    return records;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    try {
      records.push_back(nlohmann::json::parse(line));
    } catch (...) {
      // Ignore parse errors to keep loading other records
    }
  }
  svp::core::check_memory_limit("index.read_jsonl.end", {
      {"path", path.string()},
      {"records", std::to_string(records.size())}
  });
  return records;
}

std::string blake3_hex_for_file(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
    return "blake3:af1349b9f5f9a1a6a040414f808c47f942896582987abaaae1f0110de8e34890";
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return "blake3:af1349b9f5f9a1a6a040414f808c47f942896582987abaaae1f0110de8e34890";
  }

  blake3_hasher hasher;
  blake3_hasher_init(&hasher);

  std::array<char, 64 * 1024> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = file.gcount();
    if (count > 0) {
      blake3_hasher_update(&hasher, buffer.data(), static_cast<size_t>(count));
    }
  }

  std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());

  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return "blake3:" + stream.str();
}

std::string blake3_hex_for_string(const std::string& data) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data.data(), data.size());
  std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());

  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    stream << std::setw(2) << static_cast<int>(byte);
  }
  return "blake3:" + stream.str();
}

void bind_json_string(sqlite3_stmt* stmt, int col, const nlohmann::json& val, const std::string& key) {
  const auto it = val.find(key);
  if (it != val.end() && !it->is_null()) {
    sqlite3_bind_text(stmt, col, it->get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, col);
  }
}

void bind_json_int(sqlite3_stmt* stmt, int col, const nlohmann::json& val, const std::string& key) {
  const auto it = val.find(key);
  if (it != val.end() && !it->is_null() && it->is_number_integer()) {
    sqlite3_bind_int64(stmt, col, it->get<std::int64_t>());
  } else {
    sqlite3_bind_null(stmt, col);
  }
}

} // namespace

bool write_index_foundation(
    const std::filesystem::path& staging_dir,
    const nlohmann::json& manifest_json) {
  try {
    svp::core::check_memory_limit("index.write.begin", {
        {"staging_dir", staging_dir.string()}
    });
    const std::filesystem::path index_dir = staging_dir / "index";
    std::filesystem::create_directories(index_dir);
    const std::filesystem::path sqlite_path = index_dir / "index.sqlite";

    // Recreate sqlite file atomically/cleanly
    if (std::filesystem::exists(sqlite_path)) {
      std::filesystem::remove(sqlite_path);
    }

    sqlite3* raw_db = nullptr;
    if (sqlite3_open(sqlite_path.string().c_str(), &raw_db) != SQLITE_OK) {
      std::cerr << "failed to open sqlite database: " << (raw_db ? sqlite3_errmsg(raw_db) : "") << "\n";
      if (raw_db) sqlite3_close(raw_db);
      return false;
    }
    std::unique_ptr<sqlite3, SqliteDeleter> db{raw_db};
    svp::core::check_memory_limit("index.sqlite.opened", {
        {"sqlite_path", sqlite_path.string()}
    });

    // Create tables
    for (const auto& sql : kIndexTables) {
      char* err = nullptr;
      if (sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "failed to create table: " << (err ? err : "") << "\n";
        if (err) sqlite3_free(err);
        return false;
      }
    }

    // Insert svp_meta entries
    {
      const std::string meta_sql = "INSERT INTO svp_meta (key, value) VALUES (?, ?)";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db.get(), meta_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_guard{stmt};

        const auto pkg_it = manifest_json.find("package_id");
        if (pkg_it != manifest_json.end()) {
          sqlite3_bind_text(stmt, 1, "package_id", -1, SQLITE_STATIC);
          sqlite3_bind_text(stmt, 2, pkg_it->get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_step(stmt);
          sqlite3_reset(stmt);
        }

        const auto created_it = manifest_json.find("created_utc");
        if (created_it != manifest_json.end()) {
          sqlite3_bind_text(stmt, 1, "created_utc", -1, SQLITE_STATIC);
          sqlite3_bind_text(stmt, 2, created_it->get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_step(stmt);
        }
      }
    }

    // Load and insert text records
    struct TimeRange {
      std::optional<std::int64_t> start_us;
      std::optional<std::int64_t> end_us;
    };
    std::unordered_map<std::string, TimeRange> region_times;

    const auto text_regions = read_jsonl(staging_dir / "text" / "text_regions.jsonl");
    if (!text_regions.empty()) {
      const std::string insert_sql =
          "INSERT INTO text_regions (text_region_id, start_us, end_us, shot_id, scene_id) "
          "VALUES (?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_guard{stmt};
        for (const auto& reg : text_regions) {
          const auto region_id = reg.value("text_region_id", "");
          if (region_id.empty()) continue;

          sqlite3_bind_text(stmt, 1, region_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_int(stmt, 2, reg, "start_us");
          bind_json_int(stmt, 3, reg, "end_us");
          bind_json_string(stmt, 4, reg, "shot_id");
          bind_json_string(stmt, 5, reg, "scene_id");

          sqlite3_step(stmt);
          sqlite3_reset(stmt);

          TimeRange range;
          if (reg.contains("start_us") && !reg["start_us"].is_null()) {
            range.start_us = reg["start_us"].get<std::int64_t>();
          }
          if (reg.contains("end_us") && !reg["end_us"].is_null()) {
            range.end_us = reg["end_us"].get<std::int64_t>();
          }
          region_times[region_id] = range;
        }
      }
    }

    const auto text_observations = read_jsonl(staging_dir / "text" / "text_observations.jsonl");
    if (!text_observations.empty()) {
      const std::string insert_obs_sql =
          "INSERT INTO text_observations (text_observation_id, text_region_id, raw_text, "
          "normalized_text, layout_class) VALUES (?, ?, ?, ?, ?)";
      const std::string insert_fts_sql =
          "INSERT INTO text_fts (object_id, object_type, start_us, end_us, text) "
          "VALUES (?, ?, ?, ?, ?)";

      sqlite3_stmt* stmt_obs = nullptr;
      sqlite3_stmt* stmt_fts = nullptr;

      if (sqlite3_prepare_v2(db.get(), insert_obs_sql.c_str(), -1, &stmt_obs, nullptr) == SQLITE_OK &&
          sqlite3_prepare_v2(db.get(), insert_fts_sql.c_str(), -1, &stmt_fts, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_obs_guard{stmt_obs};
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_fts_guard{stmt_fts};

        for (const auto& obs : text_observations) {
          const auto obs_id = obs.value("text_observation_id", "");
          const auto region_id = obs.value("text_region_id", "");
          if (obs_id.empty() || region_id.empty()) continue;

          // Insert into text_observations
          sqlite3_bind_text(stmt_obs, 1, obs_id.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_obs, 2, region_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_obs, 3, obs, "raw_text");
          bind_json_string(stmt_obs, 4, obs, "normalized_text");
          bind_json_string(stmt_obs, 5, obs, "layout_class");

          sqlite3_step(stmt_obs);
          sqlite3_reset(stmt_obs);

          // Insert into text_fts
          sqlite3_bind_text(stmt_fts, 1, obs_id.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt_fts, 2, "text_observation", -1, SQLITE_STATIC);

          const auto time_it = region_times.find(region_id);
          if (time_it != region_times.end()) {
            if (time_it->second.start_us.has_value()) {
              sqlite3_bind_int64(stmt_fts, 3, *time_it->second.start_us);
            } else {
              sqlite3_bind_null(stmt_fts, 3);
            }
            if (time_it->second.end_us.has_value()) {
              sqlite3_bind_int64(stmt_fts, 4, *time_it->second.end_us);
            } else {
              sqlite3_bind_null(stmt_fts, 4);
            }
          } else {
            sqlite3_bind_null(stmt_fts, 3);
            sqlite3_bind_null(stmt_fts, 4);
          }

          bind_json_string(stmt_fts, 5, obs, "normalized_text");

          sqlite3_step(stmt_fts);
          sqlite3_reset(stmt_fts);
        }
      } else {
        if (stmt_obs) sqlite3_finalize(stmt_obs);
        if (stmt_fts) sqlite3_finalize(stmt_fts);
      }
    }

    const auto numeric_values = read_jsonl(staging_dir / "text" / "numeric_values.jsonl");
    if (!numeric_values.empty()) {
      const std::string insert_sql =
          "INSERT INTO numeric_values (numeric_value_id, text_observation_id, text_region_id, "
          "numeric_value, raw_text, normalized_text) VALUES (?, ?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_guard{stmt};
        for (const auto& num : numeric_values) {
          const auto num_id = num.value("numeric_value_id", "");
          if (num_id.empty()) continue;

          sqlite3_bind_text(stmt, 1, num_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt, 2, num, "text_observation_id");
          bind_json_string(stmt, 3, num, "text_region_id");
          bind_json_string(stmt, 4, num, "numeric_value");
          bind_json_string(stmt, 5, num, "raw_text");
          bind_json_string(stmt, 6, num, "normalized_text");

          sqlite3_step(stmt);
          sqlite3_reset(stmt);
        }
      }
    }

    // Load and insert color records
    const auto color_observations = read_jsonl(staging_dir / "colors" / "color_observations.jsonl");
    if (!color_observations.empty()) {
      const std::string insert_obs_sql =
          "INSERT INTO color_observations (color_observation_id, target_type, target_id, dominant_bucket) "
          "VALUES (?, ?, ?, ?)";
      const std::string insert_target_sql =
          "INSERT INTO color_targets (color_observation_id, target_type, target_id) "
          "VALUES (?, ?, ?)";
      const std::string insert_cov_sql =
          "INSERT INTO color_bucket_coverage (color_observation_id, bucket_id, coverage) "
          "VALUES (?, ?, ?)";

      sqlite3_stmt* stmt_obs = nullptr;
      sqlite3_stmt* stmt_target = nullptr;
      sqlite3_stmt* stmt_cov = nullptr;

      if (sqlite3_prepare_v2(db.get(), insert_obs_sql.c_str(), -1, &stmt_obs, nullptr) == SQLITE_OK &&
          sqlite3_prepare_v2(db.get(), insert_target_sql.c_str(), -1, &stmt_target, nullptr) == SQLITE_OK &&
          sqlite3_prepare_v2(db.get(), insert_cov_sql.c_str(), -1, &stmt_cov, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_obs_guard{stmt_obs};
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_target_guard{stmt_target};
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_cov_guard{stmt_cov};

        for (const auto& color : color_observations) {
          const auto color_id = color.value("color_observation_id", "");
          if (color_id.empty()) continue;

          // Insert color_observations
          sqlite3_bind_text(stmt_obs, 1, color_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_obs, 2, color, "target_type");
          bind_json_string(stmt_obs, 3, color, "target_id");
          bind_json_string(stmt_obs, 4, color, "dominant_bucket");
          sqlite3_step(stmt_obs);
          sqlite3_reset(stmt_obs);

          // Insert color_targets
          sqlite3_bind_text(stmt_target, 1, color_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_target, 2, color, "target_type");
          bind_json_string(stmt_target, 3, color, "target_id");
          sqlite3_step(stmt_target);
          sqlite3_reset(stmt_target);

          // Insert color_bucket_coverage
          if (color.contains("bucket_coverage") && color["bucket_coverage"].is_object()) {
            for (const auto& [bucket_id, coverage_val] : color["bucket_coverage"].items()) {
              if (coverage_val.is_number()) {
                sqlite3_bind_text(stmt_cov, 1, color_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt_cov, 2, bucket_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt_cov, 3, coverage_val.get<double>());
                sqlite3_step(stmt_cov);
                sqlite3_reset(stmt_cov);
              }
            }
          }
        }
      } else {
        if (stmt_obs) sqlite3_finalize(stmt_obs);
        if (stmt_target) sqlite3_finalize(stmt_target);
        if (stmt_cov) sqlite3_finalize(stmt_cov);
      }
    }

    // Load and insert entity records
    const auto entity_records = read_jsonl(staging_dir / "entities" / "entities.jsonl");
    if (!entity_records.empty()) {
      const std::string insert_entity_sql =
          "INSERT INTO entities (entity_id, entity_type, first_seen_us, last_seen_us) "
          "VALUES (?, ?, ?, ?)";
      sqlite3_stmt* stmt_entity = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_entity_sql.c_str(), -1, &stmt_entity, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_entity_guard{stmt_entity};
        for (const auto& entity : entity_records) {
          const auto entity_id = entity.value("id", "");
          if (entity_id.empty()) continue;
          sqlite3_bind_text(stmt_entity, 1, entity_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_entity, 2, entity, "entity_type");
          bind_json_int(stmt_entity, 3, entity, "first_seen_us");
          bind_json_int(stmt_entity, 4, entity, "last_seen_us");
          sqlite3_step(stmt_entity);
          sqlite3_reset(stmt_entity);
        }
      } else {
        if (stmt_entity) sqlite3_finalize(stmt_entity);
      }
    }

    // Load and insert entity track records
    const auto track_records = read_jsonl(staging_dir / "entities" / "entity_tracks.jsonl");
    if (!track_records.empty()) {
      const std::string insert_track_sql =
          "INSERT INTO entity_tracks (track_id, entity_id, start_us, end_us, confidence) "
          "VALUES (?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt_track = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_track_sql.c_str(), -1, &stmt_track, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_track_guard{stmt_track};
        for (const auto& track : track_records) {
          const auto track_id = track.value("id", "");
          if (track_id.empty()) continue;
          sqlite3_bind_text(stmt_track, 1, track_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_track, 2, track, "entity_id");
          bind_json_int(stmt_track, 3, track, "start_us");
          bind_json_int(stmt_track, 4, track, "end_us");
          const auto conf_it = track.find("confidence");
          if (conf_it != track.end() && conf_it->is_number()) {
            sqlite3_bind_double(stmt_track, 5, conf_it->get<double>());
          } else {
            sqlite3_bind_null(stmt_track, 5);
          }
          sqlite3_step(stmt_track);
          sqlite3_reset(stmt_track);
        }
      } else {
        if (stmt_track) sqlite3_finalize(stmt_track);
      }
    }

    // Load and insert spatial region records
    const auto region_records = read_jsonl(staging_dir / "spatial" / "regions.jsonl");
    if (!region_records.empty()) {
      const std::string insert_region_sql =
          "INSERT INTO spatial_regions (region_id, entity_id, track_id, "
          "frame_id, pts_us, screen_area_ratio, confidence) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt_region = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_region_sql.c_str(), -1, &stmt_region, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_region_guard{stmt_region};
        for (const auto& region : region_records) {
          const auto region_id = region.value("id", "");
          if (region_id.empty()) continue;
          sqlite3_bind_text(stmt_region, 1, region_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_region, 2, region, "entity_id");
          bind_json_string(stmt_region, 3, region, "track_id");
          bind_json_string(stmt_region, 4, region, "frame_id");
          bind_json_int(stmt_region, 5, region, "pts_us");
          const auto area_it = region.find("screen_area_ratio");
          if (area_it != region.end() && area_it->is_number()) {
            sqlite3_bind_double(stmt_region, 6, area_it->get<double>());
          } else {
            sqlite3_bind_null(stmt_region, 6);
          }
          const auto conf_it = region.find("confidence");
          if (conf_it != region.end() && conf_it->is_number()) {
            sqlite3_bind_double(stmt_region, 7, conf_it->get<double>());
          } else {
            sqlite3_bind_null(stmt_region, 7);
          }
          sqlite3_step(stmt_region);
          sqlite3_reset(stmt_region);
        }
      } else {
        if (stmt_region) sqlite3_finalize(stmt_region);
      }
    }

    // Load and insert spatial mask records
    const auto mask_records = read_jsonl(staging_dir / "spatial" / "masks.index.jsonl");
    if (!mask_records.empty()) {
      const std::string insert_mask_sql =
          "INSERT INTO spatial_masks (mask_id, entity_id, track_id, "
          "region_id, frame_id, width, height) "
          "VALUES (?, ?, ?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt_mask = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_mask_sql.c_str(), -1, &stmt_mask, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_mask_guard{stmt_mask};
        for (const auto& mask : mask_records) {
          const auto mask_id = mask.value("id", "");
          if (mask_id.empty()) continue;
          sqlite3_bind_text(stmt_mask, 1, mask_id.c_str(), -1, SQLITE_TRANSIENT);
          bind_json_string(stmt_mask, 2, mask, "entity_id");
          bind_json_string(stmt_mask, 3, mask, "track_id");
          bind_json_string(stmt_mask, 4, mask, "region_id");
          bind_json_string(stmt_mask, 5, mask, "frame_id");
          bind_json_int(stmt_mask, 6, mask, "width");
          bind_json_int(stmt_mask, 7, mask, "height");
          sqlite3_step(stmt_mask);
          sqlite3_reset(stmt_mask);
        }
      } else {
        if (stmt_mask) sqlite3_finalize(stmt_mask);
      }
    }

    const auto relationships = read_jsonl(staging_dir / "relationships" / "relationships.jsonl");
    if (!relationships.empty()) {
      const std::string insert_sql =
          "INSERT INTO relationships (relationship_id, relationship_type, "
          "relationship_class, source_id, target_id, start_us, end_us, "
          "confidence) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_guard{stmt};
        for (const auto& relationship : relationships) {
          const auto relationship_id = relationship.value("id", "");
          const auto relationship_type = relationship.value("type", "");
          const auto source_id = relationship.value("source_id", "");
          const auto target_id = relationship.value("target_id", "");
          if (relationship_id.empty() || relationship_type.empty() ||
              source_id.empty() || target_id.empty() ||
              !relationship.contains("start_us") || !relationship["start_us"].is_number_integer() ||
              !relationship.contains("end_us") || !relationship["end_us"].is_number_integer() ||
              !relationship.contains("confidence") || !relationship["confidence"].is_number()) {
            continue;
          }

          const auto rel_class = classify_relationship_type(relationship_type);
          const auto class_str = relationship_class_to_string(rel_class);

          sqlite3_bind_text(stmt, 1, relationship_id.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt, 2, relationship_type.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt, 3, class_str.data(), static_cast<int>(class_str.size()), SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt, 4, source_id.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt, 5, target_id.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64(stmt, 6, relationship["start_us"].get<std::int64_t>());
          sqlite3_bind_int64(stmt, 7, relationship["end_us"].get<std::int64_t>());
          sqlite3_bind_double(stmt, 8, relationship["confidence"].get<double>());

          sqlite3_step(stmt);
          sqlite3_reset(stmt);
        }
      }
    }

    // Load and insert embedding index records into vector_index
    const auto embedding_index_records = read_jsonl(staging_dir / "embeddings" / "embeddings.index.jsonl");
    if (!embedding_index_records.empty()) {
      const std::string insert_vec_sql =
          "INSERT INTO vector_index (embedding, object_id, object_type, "
          "embedding_set_id, start_us, end_us) VALUES (?, ?, ?, ?, ?, ?)";
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db.get(), insert_vec_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::unique_ptr<sqlite3_stmt, StatementDeleter> stmt_guard{stmt};
        for (const auto& emb : embedding_index_records) {
          const auto emb_id = emb.value("id", "");
          if (emb_id.empty()) continue;

          // embedding BLOB is NULL — actual vectors live in the binary block stream;
          // the regular table stores metadata only (sqlite-vec is not linked).
          sqlite3_bind_null(stmt, 1);

          // object_id comes from input_ref (the source observation ID)
          bind_json_string(stmt, 2, emb, "input_ref");
          // object_type comes from input_kind (e.g. "text_observation")
          bind_json_string(stmt, 3, emb, "input_kind");
          bind_json_string(stmt, 4, emb, "embedding_set_id");
          bind_json_int(stmt, 5, emb, "start_us");
          bind_json_int(stmt, 6, emb, "end_us");

          sqlite3_step(stmt);
          sqlite3_reset(stmt);
        }
      }
    }

    // Retrieve user tables to compute logical rows stream digest
    std::set<std::string> table_names = {
        "binary_blocks",
        "color_bucket_coverage",
        "color_observations",
        "color_targets",
        "entities",
        "entity_tracks",
        "numeric_values",
        "objects",
        "relationships",
        "spatial_masks",
        "spatial_regions",
        "svp_meta",
        "temporal_spans",
        "text_fts",
        "text_observations",
        "text_regions",
        "vector_index"
    };

    const LogicalRowStreamSummary stream = compute_logical_row_stream_summary(*db, table_names);
    svp::core::check_memory_limit("index.logical_rows.complete", {
        {"table_count", std::to_string(stream.table_count)},
        {"row_count", std::to_string(stream.row_count)}
    });

    // Close SQLite file cleanly so file blake3 matches final bytes
    db.reset();
    svp::core::check_memory_limit("index.sqlite.closed", {
        {"sqlite_path", sqlite_path.string()}
    });

    // Compute file hashes
    const std::string sqlite_file_blake3 = blake3_hex_for_file(sqlite_path);
    const std::string manifest_content = manifest_json.dump(2) + "\n";
    const std::string manifest_blake3 = blake3_hex_for_string(manifest_content);
    const std::string binary_blocks_manifest_blake3 = blake3_hex_for_file(staging_dir / "binary_blocks_manifest.json");
    const std::string embedding_sets_blake3 = blake3_hex_for_file(staging_dir / "embeddings" / "embedding_sets.json");

    // Write index_manifest.json
    nlohmann::json index_manifest_json = {
        {"schema_version", "svp-index-manifest-v1"},
        {"index_schema_version", "svp-index-v1"},
        {"sqlite_file", "index/index.sqlite"},
        {"sqlite_file_blake3", sqlite_file_blake3},
        {"logical_row_stream_version", "svp-logical-row-stream-v1"},
        {"logical_rows_blake3", stream.blake3},
        {"table_count", stream.table_count},
        {"row_count", stream.row_count},
        {"created_from", {
            {"manifest_blake3", manifest_blake3},
            {"binary_blocks_manifest_blake3", binary_blocks_manifest_blake3},
            {"embedding_sets_blake3", embedding_sets_blake3}
        }}
    };

    std::ofstream manifest_out(index_dir / "index_manifest.json");
    if (!manifest_out) {
      return false;
    }
    manifest_out << index_manifest_json.dump(2) << "\n";

    svp::core::check_memory_limit("index.write.complete", {
        {"sqlite_path", sqlite_path.string()},
        {"row_count", std::to_string(stream.row_count)}
    });
    return true;

  } catch (const std::exception& error) {
    svp::core::trace_memory_event("index.write.exception", {
        {"error", error.what()}
    });
    std::cerr << "write_index_foundation error: " << error.what() << "\n";
    return false;
  }
}

} // namespace svp::package
