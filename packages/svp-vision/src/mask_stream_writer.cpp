#include "svp/vision/mask_writer.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace svp::vision {
namespace {

std::string hash_to_hex(const std::array<std::uint8_t, 32>& hash) {
  std::ostringstream stream;
  for (const auto byte : hash) {
    stream << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(byte);
  }
  return stream.str();
}

}  // namespace

struct MaskStreamWriter::Impl {
  explicit Impl(const std::filesystem::path& staging_dir,
                bool retain_summary_records)
      : retain_summary_records(retain_summary_records) {
    const auto spatial_dir = staging_dir / "spatial";
    std::filesystem::create_directories(spatial_dir);
    index_path = spatial_dir / "masks.index.jsonl";
    block_path = spatial_dir / "masks.blocks.svpmz";
    index_tmp_path = spatial_dir / "masks.index.jsonl.tmp";
    block_tmp_path = spatial_dir / "masks.blocks.svpmz.tmp";
    index_tmp.open(index_tmp_path);
    block_tmp.open(block_tmp_path, std::ios::binary);
    if (!index_tmp || !block_tmp) {
      throw std::runtime_error("failed to open streamed mask artifacts");
    }
  }

  std::filesystem::path index_path;
  std::filesystem::path block_path;
  std::filesystem::path index_tmp_path;
  std::filesystem::path block_tmp_path;
  std::ofstream index_tmp;
  std::ofstream block_tmp;
  MaskWriteSummary summary;
  bool retain_summary_records = false;
  bool finished = false;
};

MaskStreamWriter::MaskStreamWriter(const std::filesystem::path& staging_dir,
                                   bool retain_summary_records)
    : impl_(std::make_unique<Impl>(staging_dir, retain_summary_records)) {}

MaskStreamWriter::~MaskStreamWriter() {
  if (!impl_ || impl_->finished)
    return;
  impl_->index_tmp.close();
  impl_->block_tmp.close();
  std::error_code error;
  std::filesystem::remove(impl_->index_tmp_path, error);
  std::filesystem::remove(impl_->block_tmp_path, error);
}

MaskStreamWriter::MaskStreamWriter(MaskStreamWriter&&) noexcept = default;
MaskStreamWriter&
MaskStreamWriter::operator=(MaskStreamWriter&&) noexcept = default;

void MaskStreamWriter::append(const MaskWriteEntry& mask) {
  if (!impl_ || impl_->finished) {
    throw std::logic_error("cannot append to a finished mask stream");
  }
  svp::blocks::BlockWriteSpec spec;
  spec.block_type = svp::blocks::BlockType::mask;
  spec.extent_0 = static_cast<std::uint32_t>(mask.width);
  spec.extent_1 = static_cast<std::uint32_t>(mask.height);
  spec.extent_2 = 1;
  spec.dtype = svp::blocks::DType::svp_rle_v1;
  spec.frame_count = 1;
  const auto block_info = svp::blocks::write_block_to_stream(
      impl_->block_tmp, spec,
      reinterpret_cast<const std::byte*>(mask.rle_data.data()),
      mask.rle_data.size());

  nlohmann::json index_record = {
      {"id", mask.mask_id},
      {"region_id", mask.region_id},
      {"frame_id", mask.frame_id},
      {"width", mask.width},
      {"height", mask.height},
      {"encoding", "svp-rle-v1"},
      {"block_file", "spatial/masks.blocks.svpmz"},
      {"block_offset", block_info.block_offset},
      {"block_length", block_info.block_length},
      {"payload_offset", block_info.payload_offset},
      {"uncompressed_size", block_info.uncompressed_size},
      {"compressed_size", block_info.compressed_size},
      {"payload_blake3", hash_to_hex(block_info.payload_blake3)},
      {"block_blake3", hash_to_hex(block_info.header_blake3)}};
  nlohmann::json block_record = {
      {"block_id", mask.mask_id},
      {"block_type", "mask"},
      {"block_file", "spatial/masks.blocks.svpmz"},
      {"block_offset", block_info.block_offset},
      {"block_length", block_info.block_length},
      {"payload_offset", block_info.payload_offset},
      {"uncompressed_size", block_info.uncompressed_size},
      {"compressed_size", block_info.compressed_size},
      {"extent_0", static_cast<std::uint32_t>(mask.width)},
      {"extent_1", static_cast<std::uint32_t>(mask.height)},
      {"extent_2", 1},
      {"dtype", static_cast<std::uint32_t>(svp::blocks::DType::svp_rle_v1)},
      {"start_frame", 0},
      {"frame_count", 1},
      {"start_us", -1},
      {"end_us", -1},
      {"payload_blake3", "blake3:" + hash_to_hex(block_info.payload_blake3)},
      {"header_blake3", "blake3:" + hash_to_hex(block_info.header_blake3)},
      {"block_blake3", "blake3:" + hash_to_hex(block_info.payload_blake3)}};
  impl_->index_tmp << nlohmann::json{{"entity_id", mask.entity_id},
                                     {"index_record", index_record},
                                     {"block_record", block_record}}
                          .dump()
                   << '\n';
}

MaskWriteSummary
MaskStreamWriter::finish(const std::set<std::string>* retained_entity_ids) {
  if (!impl_ || impl_->finished) {
    throw std::logic_error("mask stream was already finished");
  }
  impl_->index_tmp.close();
  impl_->block_tmp.close();

  std::ifstream input(impl_->index_tmp_path);
  std::ofstream output(impl_->index_path);
  std::ifstream block_input(impl_->block_tmp_path, std::ios::binary);
  std::ofstream block_output(impl_->block_path, std::ios::binary);
  if (!input || !output || !block_input || !block_output) {
    throw std::runtime_error("failed to finalize streamed mask index");
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto staged = nlohmann::json::parse(line);
    const auto entity_id = staged.at("entity_id").get<std::string>();
    if (retained_entity_ids != nullptr &&
        retained_entity_ids->count(entity_id) == 0) {
      continue;
    }
    auto index_record = staged.at("index_record");
    auto block_record = staged.at("block_record");
    const auto old_block_offset =
        index_record.at("block_offset").get<std::uint64_t>();
    const auto old_payload_offset =
        index_record.at("payload_offset").get<std::uint64_t>();
    const auto block_length =
        index_record.at("block_length").get<std::uint64_t>();
    std::vector<char> block_bytes(static_cast<std::size_t>(block_length));
    block_input.seekg(static_cast<std::streamoff>(old_block_offset));
    block_input.read(block_bytes.data(),
                     static_cast<std::streamsize>(block_bytes.size()));
    if (block_input.gcount() !=
        static_cast<std::streamsize>(block_bytes.size())) {
      throw std::runtime_error("failed to read a staged mask block");
    }
    const auto new_block_offset =
        static_cast<std::uint64_t>(block_output.tellp());
    block_output.write(block_bytes.data(),
                       static_cast<std::streamsize>(block_bytes.size()));
    const auto new_payload_offset =
        new_block_offset + old_payload_offset - old_block_offset;
    index_record["block_offset"] = new_block_offset;
    index_record["payload_offset"] = new_payload_offset;
    block_record["block_offset"] = new_block_offset;
    block_record["payload_offset"] = new_payload_offset;
    output << index_record.dump() << '\n';
    ++impl_->summary.mask_count;
    if (impl_->retain_summary_records) {
      impl_->summary.index_records.push_back(index_record);
      impl_->summary.block_manifest_entries.push_back(block_record);
    }
  }
  input.close();
  output.close();
  block_input.close();
  block_output.close();
  std::filesystem::remove(impl_->index_tmp_path);
  std::filesystem::remove(impl_->block_tmp_path);
  impl_->summary.masks_index_path = impl_->index_path.string();
  impl_->summary.masks_block_path = impl_->block_path.string();
  impl_->finished = true;
  return std::move(impl_->summary);
}

}  // namespace svp::vision
