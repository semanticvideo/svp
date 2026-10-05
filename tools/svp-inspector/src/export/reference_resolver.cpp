#include "reference_resolver.hpp"

#include "jsonl_line_reader.hpp"
#include "jsonl_record.hpp"

#include <algorithm>
#include <functional>
#include <string_view>
#include <utility>

namespace package_export {
namespace {

nlohmann::json member_or_null(const nlohmann::json& object,
                              std::string_view name) {
  const auto found = object.find(std::string{name});
  return found == object.end() ? nlohmann::json(nullptr) : *found;
}

const std::string* string_member(const nlohmann::json& object,
                                 std::string_view name) {
  const auto found = object.find(std::string{name});
  if (found == object.end() || !found->is_string()) {
    return nullptr;
  }
  return &found->get_ref<const std::string&>();
}

bool is_string_array(const nlohmann::json& value) {
  return value.is_array() &&
         std::ranges::all_of(value, [](const nlohmann::json& element) {
           return element.is_string();
         });
}

// Streams one lookup layer and hands every record to `visit`.
std::uint64_t for_each_record(
    const PackageReader& reader, const PackageEntry& entry,
    const std::function<void(const nlohmann::json&)>& visit) {
  JsonlLineReader lines{reader.open(entry), entry.name};
  std::string_view text;
  std::uint64_t line = 0;
  std::uint64_t count = 0;
  while (lines.next(text, line)) {
    visit(parse_record(text, entry.name, line));
    ++count;
  }
  return count;
}

nlohmann::json source_json(std::string_view entry, bool present,
                           std::uint64_t record_count) {
  return nlohmann::json{
      {"entry", std::string{entry}},
      {"present", present},
      {"record_count", present ? nlohmann::json(record_count)
                               : nlohmann::json(nullptr)},
  };
}

}  // namespace

ReferenceResolver::ReferenceResolver(const ExportPlan& plan) : plan_(plan) {}

void ReferenceResolver::load_lookups(const PackageReader& reader) {
  if (const auto* frames = reader.find_file(kFramesLayer)) {
    frames_source_.present = true;
    frames_source_.record_count =
        for_each_record(reader, *frames, [&](const nlohmann::json& record) {
          const auto* id = string_member(record, kFrameIdMember);
          if (id == nullptr) {
            return;
          }
          // The first record with an id wins; later duplicates are ignored.
          frames_.try_emplace(*id,
                              FrameValues{
                                  member_or_null(record, kFrameIndexMember),
                                  member_or_null(record, kFramePtsMember),
                              });
        });
  }
  if (const auto* crops = reader.find_file(kEvidenceCropsLayer)) {
    crops_source_.present = true;
    crops_source_.record_count =
        for_each_record(reader, *crops, [&](const nlohmann::json& record) {
          const auto* id = string_member(record, kCropIdMember);
          if (id == nullptr) {
            return;
          }
          crop_paths_.try_emplace(*id,
                                  member_or_null(record, kCropFilePathMember));
        });
  }
}

void ReferenceResolver::add_block_stream(DecodedBlockStream stream) {
  auto entry = stream.entry;
  block_streams_.insert_or_assign(std::move(entry), std::move(stream));
}

nlohmann::json ReferenceResolver::exported_file_for(
    const std::string& path) const {
  const auto* layer = plan_.find(path);
  if (layer == nullptr ||
      layer->representation == Representation::block_stream) {
    return nullptr;
  }
  return layer_file_path(path);
}

nlohmann::json ReferenceResolver::resolve_frame(const std::string& id) const {
  const auto found = frames_.find(id);
  if (found == frames_.end()) {
    return {{"id", id}, {"found", false}};
  }
  return {
      {"id", id},
      {"found", true},
      {"frame_index", found->second.frame_index},
      {"pts_us", found->second.pts_us},
  };
}

nlohmann::json ReferenceResolver::resolve_crop(const std::string& id) const {
  const auto found = crop_paths_.find(id);
  if (found == crop_paths_.end()) {
    return {{"id", id}, {"found", false}};
  }
  const auto& path = found->second;
  return {
      {"id", id},
      {"found", true},
      {"file", path.is_string()
                   ? exported_file_for(path.get_ref<const std::string&>())
                   : nlohmann::json(nullptr)},
  };
}

nlohmann::json ReferenceResolver::resolve_entry_path(
    const std::string& path) const {
  auto file = exported_file_for(path);
  const bool found = !file.is_null();
  return {{"path", path}, {"found", found}, {"file", std::move(file)}};
}

nlohmann::json ReferenceResolver::resolve_block(
    const nlohmann::json& record, const std::string& block_file) const {
  const auto declared_offset = member_or_null(record, kBlockOffsetMember);
  nlohmann::json resolution{
      {"block_file", block_file},
      {"block_offset", declared_offset},
      {"found", false},
  };
  const auto stream = block_streams_.find(block_file);
  if (stream == block_streams_.end() ||
      !declared_offset.is_number_unsigned()) {
    return resolution;
  }
  const auto* block =
      stream->second.find_by_offset(declared_offset.get<std::uint64_t>());
  if (block == nullptr) {
    return resolution;
  }
  const auto& header = block->header;
  const auto dtype = dtype_name(header.dtype);
  resolution["found"] = true;
  resolution["block_ordinal"] = block->ordinal;
  resolution["decoded_file"] = stream->second.decoded_file;
  resolution["decoded_offset"] = block->decoded_offset;
  resolution["decoded_length"] = header.uncompressed_size;
  resolution["dtype"] = header.dtype;
  resolution["dtype_name"] =
      dtype.empty() ? nlohmann::json(nullptr) : nlohmann::json(dtype);
  resolution["extents"] = {header.extent_0, header.extent_1, header.extent_2};
  return resolution;
}

nlohmann::json ReferenceResolver::resolve(
    const nlohmann::json& record, std::span<const ReferenceRule> rules) const {
  auto resolutions = nlohmann::json::object();
  for (const auto& rule : rules) {
    const std::string member{rule.member};
    const auto found = record.find(member);
    if (found == record.end()) {
      continue;
    }
    const auto& value = *found;
    switch (rule.kind) {
      case ReferenceKind::frame:
        if (value.is_string()) {
          resolutions[member] =
              resolve_frame(value.get_ref<const std::string&>());
        }
        break;
      case ReferenceKind::crop:
        if (value.is_string()) {
          resolutions[member] =
              resolve_crop(value.get_ref<const std::string&>());
        }
        break;
      case ReferenceKind::frame_list:
      case ReferenceKind::crop_list:
        if (is_string_array(value)) {
          auto list = nlohmann::json::array();
          for (const auto& element : value) {
            const auto& id = element.get_ref<const std::string&>();
            list.push_back(rule.kind == ReferenceKind::frame_list
                               ? resolve_frame(id)
                               : resolve_crop(id));
          }
          resolutions[member] = std::move(list);
        }
        break;
      case ReferenceKind::entry_path:
        if (value.is_string()) {
          resolutions[member] =
              resolve_entry_path(value.get_ref<const std::string&>());
        }
        break;
      case ReferenceKind::block:
        if (value.is_string()) {
          resolutions[member] =
              resolve_block(record, value.get_ref<const std::string&>());
        }
        break;
    }
  }
  return resolutions;
}

nlohmann::json ReferenceResolver::sources_json() const {
  return {
      {"frames", source_json(kFramesLayer, frames_source_.present,
                             frames_source_.record_count)},
      {"evidence_crops",
       source_json(kEvidenceCropsLayer, crops_source_.present,
                   crops_source_.record_count)},
  };
}

}  // namespace package_export
