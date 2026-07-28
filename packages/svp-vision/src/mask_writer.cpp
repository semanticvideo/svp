#include "svp/vision/mask_writer.hpp"

namespace svp::vision {

MaskWriteSummary write_masks(
    const std::filesystem::path& staging_dir,
    const std::vector<MaskWriteEntry>& masks) {
  MaskStreamWriter writer(staging_dir, true);
  for (const auto& mask : masks) writer.append(mask);
  return writer.finish();
}

}  // namespace svp::vision
