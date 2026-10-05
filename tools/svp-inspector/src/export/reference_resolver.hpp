#pragma once

#include "block_stream_export.hpp"
#include "export_plan.hpp"
#include "package_reader.hpp"
#include "reference_rules.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace package_export {

// Resolves the record members listed in the reference registry to the values
// they point at (Package_Export_v1.md Section 5). Lookups are loaded once
// before any layer is exported; resolution itself never fails.
class ReferenceResolver {
 public:
  explicit ReferenceResolver(const ExportPlan& plan);

  // Loads timeline/frames.jsonl and text/evidence_crops.jsonl when present.
  // Malformed lookup records are refused the same way exporting them would be.
  void load_lookups(const PackageReader& reader);

  void add_block_stream(DecodedBlockStream stream);

  // The `svp_export` object for one record; empty when no rule applies.
  [[nodiscard]] nlohmann::json resolve(
      const nlohmann::json& record,
      std::span<const ReferenceRule> rules) const;

  // export.json `resolution_sources`.
  [[nodiscard]] nlohmann::json sources_json() const;

 private:
  struct FrameValues {
    nlohmann::json frame_index;
    nlohmann::json pts_us;
  };

  struct LookupSource {
    bool present = false;
    std::uint64_t record_count = 0;
  };

  [[nodiscard]] nlohmann::json resolve_frame(const std::string& id) const;
  [[nodiscard]] nlohmann::json resolve_crop(const std::string& id) const;
  [[nodiscard]] nlohmann::json resolve_entry_path(const std::string& path) const;
  [[nodiscard]] nlohmann::json resolve_block(const nlohmann::json& record,
                                             const std::string& block_file) const;
  [[nodiscard]] nlohmann::json exported_file_for(const std::string& path) const;

  const ExportPlan& plan_;
  std::map<std::string, FrameValues, std::less<>> frames_;
  std::map<std::string, nlohmann::json, std::less<>> crop_paths_;
  std::map<std::string, DecodedBlockStream, std::less<>> block_streams_;
  LookupSource frames_source_;
  LookupSource crops_source_;
};

}  // namespace package_export
