#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace svp::vision {

struct TokenizedText {
  std::vector<std::int64_t> input_ids;
  std::vector<std::int64_t> token_type_ids;
  std::vector<std::int64_t> attention_mask;
  std::size_t seq_len = 0;
};

class WordPieceTokenizer {
 public:
  [[nodiscard]] bool load(const std::filesystem::path& vocab_path);

  [[nodiscard]] bool is_loaded() const { return !vocab_.empty(); }

  [[nodiscard]] TokenizedText tokenize(
      const std::string& text,
      std::size_t max_length = 512) const;

 private:
  std::unordered_map<std::string, std::int64_t> vocab_;
  std::int64_t cls_id_ = 101;
  std::int64_t sep_id_ = 102;
  std::int64_t pad_id_ = 0;
  std::int64_t unk_id_ = 100;

  [[nodiscard]] std::vector<std::string> basic_tokenize(
      const std::string& text) const;
  [[nodiscard]] std::vector<std::string> wordpiece_tokenize(
      const std::string& word) const;
  [[nodiscard]] std::string to_lower(const std::string& text) const;
};

}  // namespace svp::vision
