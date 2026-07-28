#pragma once

#include "svp/vision/mask_writer.hpp"
#include "svp/vision/visual_entity_tracker.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace svp::package {

struct VisualEntityArtifactStreamSummary {
  std::size_t region_count = 0;
  std::size_t mask_count = 0;
};

class VisualEntityArtifactWriter {
 public:
  explicit VisualEntityArtifactWriter(const std::filesystem::path& staging_dir);
  ~VisualEntityArtifactWriter();

  VisualEntityArtifactWriter(const VisualEntityArtifactWriter&) = delete;
  VisualEntityArtifactWriter&
  operator=(const VisualEntityArtifactWriter&) = delete;

  void append(const std::vector<svp::vision::TrackedRegion>& regions,
              const std::vector<svp::vision::MaskWriteEntry>& masks);
  [[nodiscard]] VisualEntityArtifactStreamSummary
  finish(const std::set<std::string>& retained_entity_ids);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace svp::package
