#include "svpi_fixture_builder.hpp"

#include "svp/blocks/block_writer.hpp"
#include "svp/package/media_binding.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace export_test {
namespace {

static_assert(std::endian::native == std::endian::little,
              "fixture payloads are written in host order");

constexpr std::uint32_t kDepthWidth = 4;
constexpr std::uint32_t kDepthHeight = 2;

std::string to_string(const std::vector<std::byte>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

template <typename Value>
std::vector<std::byte> as_bytes(const std::vector<Value>& values) {
  std::vector<std::byte> bytes(values.size() * sizeof(Value));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

svp::blocks::WrittenBlockInfo append_depth_block(
    std::vector<std::byte>& stream, const std::vector<std::uint16_t>& values,
    std::uint64_t frame, std::int64_t start_us, std::int64_t end_us) {
  const auto payload = as_bytes(values);
  return svp::blocks::write_block(
      stream,
      svp::blocks::BlockWriteSpec{
          .block_type = svp::blocks::BlockType::depth,
          .extent_0 = kDepthWidth,
          .extent_1 = kDepthHeight,
          .extent_2 = 1,
          .dtype = svp::blocks::DType::uint16,
          .start_frame = frame,
          .frame_count = 1,
          .start_us = start_us,
          .end_us = end_us,
      },
      payload.data(), payload.size());
}

std::string media_binding_json() {
  return nlohmann::json{
      {"schema", "svpi.media_binding.v0.1"},
      {"primary_binding_id", "mb_primary_000001"},
      {"bindings",
       {{
           {"binding_id", "mb_primary_000001"},
           {"binding_contract", std::string{svp::package::kSvpiBindingContract}},
           {"media_role", "primary_source"},
           {"media_id", "media_src_000001"},
           {"container_format", "mov,mp4,m4a,3gp,3g2,mj2"},
           {"verification_state", "pending"},
           {"duration_us", 4000},
           {"size_bytes", 1234},
           {"streams", nlohmann::json::array()},
           {"location_hints", {{"original_filename", "fixture.mp4"}}},
           {"identity",
            {{"full_file_blake3", {{"state", "pending"}}},
             {"chunk_hashes",
              {{"algorithm", "blake3"},
               {"chunk_size_bytes", 16},
               {"chunk_count", 1}}}}},
       }}},
  }.dump();
}

std::string manifest_json() {
  return nlohmann::json{
      {"format", "svpi"},
      {"svpi_version", std::string{svp::package::kSvpiVersion}},
      {"svp_version", "1.0-rc.2"},
      {"package_id", "svpi_export_fixture"},
      {"media_binding_ref", "media_binding.json"},
      {"primary_media_binding_id", "mb_primary_000001"},
      {"sections",
       {{"text", {{"state", "generated"}}},
        {"spatial", {{"state", "generated"}}},
        {"entities", {{"state", "not_generated"}}}}},
  }.dump();
}

}  // namespace

const std::string& entry_content(const std::vector<ZipEntrySpec>& entries,
                                 std::string_view name) {
  const auto found = std::ranges::find(entries, name, &ZipEntrySpec::name);
  if (found == entries.end()) {
    throw std::runtime_error("fixture has no entry " + std::string{name});
  }
  return found->content;
}

void set_entry(std::vector<ZipEntrySpec>& entries, std::string_view name,
               std::string content) {
  const auto found = std::ranges::find(entries, name, &ZipEntrySpec::name);
  if (found == entries.end()) {
    throw std::runtime_error("fixture has no entry " + std::string{name});
  }
  found->content = std::move(content);
}

void add_entry(std::vector<ZipEntrySpec>& entries, std::string name,
               std::string content) {
  entries.push_back({std::move(name), std::move(content), false});
}

SvpiFixture make_svpi_fixture() {
  SvpiFixture fixture;
  auto& facts = fixture.facts;
  facts.depth_block_0 = {0, 1000, 2000, 3000, 4000, 5000, 6000, 65535};
  facts.depth_block_1 = {65535, 7, 6, 5, 4, 3, 2, 0};
  facts.embedding_vector = {0.25F, -1.5F, 3.0F};
  facts.crop_bytes = std::string{"\xFF\xD8\xFF\xE0" "fake-jpeg\x00\x01\x02", 16};
  facts.extension_bytes = std::string{"\x00\x01\x02\x03vendor", 10};

  std::vector<std::byte> depth_stream;
  const auto depth_0 =
      append_depth_block(depth_stream, facts.depth_block_0, 0, 1000, 2000);
  const auto depth_1 =
      append_depth_block(depth_stream, facts.depth_block_1, 1, 2000, 3000);
  facts.depth_block_1_offset = depth_1.block_offset;

  std::vector<std::byte> embedding_stream;
  const auto vector_bytes = as_bytes(facts.embedding_vector);
  const auto embedding = svp::blocks::write_block(
      embedding_stream,
      svp::blocks::BlockWriteSpec{
          .block_type = svp::blocks::BlockType::embedding,
          .extent_0 = 1,
          .extent_1 = static_cast<std::uint32_t>(facts.embedding_vector.size()),
          .extent_2 = 1,
          .dtype = svp::blocks::DType::float32,
          .start_frame = std::numeric_limits<std::uint64_t>::max(),
          .frame_count = 0,
          .start_us = -1,
          .end_us = -1,
      },
      vector_bytes.data(), vector_bytes.size());

  const auto depth_index =
      "{\"block_file\":\"spatial/depth.blocks.svpdz\",\"block_offset\":" +
      std::to_string(depth_0.block_offset) +
      ",\"frame_id\":\"frame_000001\",\"id\":\"depth_frame_000001\"}\n"
      "{\"block_file\":\"spatial/depth.blocks.svpdz\",\"block_offset\":" +
      std::to_string(depth_1.block_offset) +
      ",\"frame_id\":\"frame_000002\",\"id\":\"depth_frame_000002\"}\n"
      "{\"block_file\":\"spatial/depth.blocks.svpdz\",\"block_offset\":7,"
      "\"frame_id\":\"frame_000003\",\"id\":\"depth_frame_000003\"}\n";
  const auto embedding_index =
      "{\"block_file\":\"embeddings/embeddings.blocks.svpez\",\"block_offset\":" +
      std::to_string(embedding.block_offset) +
      ",\"id\":\"embed_text_obs_000001\",\"vector_index\":0}\n";

  fixture.entries = {
      {"mimetype", std::string{svp::package::kSvpiMimetype}, true},
      {"manifest.json", manifest_json()},
      {"media_binding.json", media_binding_json()},
      {"timeline/frames.jsonl",
       "{\"frame_index\":0,\"id\":\"frame_000001\",\"pts_us\":1000}\n"
       "{\"frame_index\":1,\"id\":\"frame_000002\",\"pts_us\":2000}\n"
       "{\"frame_index\":2,\"id\":\"frame_000003\",\"pts_us\":3500}\n"},
      {"text/text_observations.jsonl",
       "{\"evidence_crop_refs\":[\"crop_000001\"],\"raw_text\":\"SUBTO\","
       "\"source_frame_ids\":[\"frame_000002\",\"frame_000001\","
       "\"frame_000002\"],\"text_observation_id\":\"text_obs_000001\"}\n"
       "  {\"confidence\":1.0,\"raw_text\":\"x\",\"source_frame_ids\":"
       "[\"frame_999999\"],\"text_observation_id\":\"text_obs_000002\"} \r\n"
       "\n"
       "{\"raw_text\":\"no refs\",\"text_observation_id\":\"text_obs_000003\"}\n"
       "{\"evidence_crop_refs\":[\"crop_missing\"],\"source_frame_ids\":"
       "\"frame_000001\",\"text_observation_id\":\"text_obs_000004\"}"},
      {"text/evidence_crops.jsonl",
       "{\"crop_file_path\":\"text/evidence_crops/crop_000001.jpg\","
       "\"crop_id\":\"crop_000001\",\"source_frame_id\":\"frame_000002\"}\n"
       "{\"crop_file_path\":\"text/evidence_crops/absent.jpg\","
       "\"crop_id\":\"crop_000002\",\"source_frame_id\":\"frame_000003\"}\n"},
      {"text/evidence_crops/crop_000001.jpg", facts.crop_bytes, true},
      {"transcript/words.jsonl",
       "{\"confidence\":0.8782150745391846,\"end_us\":120000,"
       "\"id\":\"word_000000\",\"speaker_id\":\"speaker_0001\","
       "\"start_us\":60000,\"text\":\"How\"}\n"
       "{\"id\":\"word_000001\",\"start_us\":1e2,"
       "\"text\":\"caf\\u00e9 \\\"q\\\"\"}\n"},
      {"provenance/processors.jsonl",
       "{\"id\":\"proc_test_0001\",\"temporal_sampling\":"
       "{\"sampled_timestamps_us\":[1000,2000,3500]}}\n"},
      {"provenance/interlace_events.jsonl",
       "{\"event_id\":\"evt_interlace_create_000001\","
       "\"event_type\":\"svpi_created_from_media\"}\n"},
      {"index/index.sqlite", std::string{"SQLite format 3\0fixture", 23}, true},
      {"index/index_manifest.json",
       "{\"schema_version\":\"svp-index-manifest-v1\"}"},
      {"colors/color_summary.json", "{\"color_observation_count\":0}"},
      {"spatial/depth.index.jsonl", depth_index},
      {"spatial/depth.blocks.svpdz", to_string(depth_stream), true},
      {"spatial/masks.index.jsonl", ""},
      {"spatial/masks.blocks.svpmz", "", true},
      {"embeddings/embeddings.index.jsonl", embedding_index},
      {"embeddings/embeddings.blocks.svpez", to_string(embedding_stream), true},
      {"extensions/vendor.example/blob.bin", facts.extension_bytes},
  };
  return fixture;
}

}  // namespace export_test
