#include "equivalence/entry_layers.hpp"

namespace svp::validation::equivalence {
namespace {

constexpr std::string_view kIndexSqliteEntry = "index/index.sqlite";
constexpr std::string_view kOriginalMediaPrefix = "media/original/";
constexpr std::string_view kAudioPrefix = "media/audio/";

// RC2 Section 14.5 block stream file extensions.
struct BlockExtension {
  std::string_view extension;
  svp::blocks::BlockType block_type;
};
constexpr BlockExtension kBlockExtensions[] = {
    {".svpdz", svp::blocks::BlockType::depth},
    {".svpmz", svp::blocks::BlockType::mask},
    {".svpez", svp::blocks::BlockType::embedding},
};

}  // namespace

EntryLayerInfo classify_entry(std::string_view entry) {
  if (entry.ends_with('/')) {
    return {.layer = EntryLayer::directory, .name = "directory"};
  }
  if (entry == kIndexSqliteEntry) {
    return {.layer = EntryLayer::sqlite_index, .name = "sqlite_logical"};
  }
  for (const auto& block : kBlockExtensions) {
    if (entry.ends_with(block.extension)) {
      return {.layer = EntryLayer::block_stream,
              .name = "block_stream",
              .block_type = block.block_type};
    }
  }
  if (entry.ends_with(".jsonl")) {
    return {.layer = EntryLayer::canonical_jsonl, .name = "canonical_jsonl"};
  }
  if (entry.ends_with(".json")) {
    return {.layer = EntryLayer::canonical_json, .name = "canonical_json"};
  }
  if (entry.starts_with(kOriginalMediaPrefix)) {
    return {.layer = EntryLayer::exact_bytes, .name = "original_media"};
  }
  if (entry.starts_with(kAudioPrefix)) {
    return {.layer = EntryLayer::exact_bytes, .name = "lossless_audio"};
  }
  return {.layer = EntryLayer::exact_bytes, .name = "exact_bytes"};
}

}  // namespace svp::validation::equivalence
