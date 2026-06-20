#include "svp/vision/text_tokenizer.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace svp::vision {

namespace {

bool is_whitespace(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool is_punctuation(char c) {
  const unsigned char uc = static_cast<unsigned char>(c);
  // ASCII punctuation ranges (same as BERT)
  if ((uc >= 33 && uc <= 47) ||
      (uc >= 58 && uc <= 64) ||
      (uc >= 91 && uc <= 96) ||
      (uc >= 123 && uc <= 126)) {
    return true;
  }
  // Unicode punctuation check (simplified: check Unicode categories)
  // For CJK and other non-ASCII, we treat as non-punctuation
  return false;
}

bool is_control_char(char c) {
  if (c == '\t' || c == '\n' || c == '\r') return false;
  const unsigned char uc = static_cast<unsigned char>(c);
  return uc < 32 || uc == 127;
}

std::string strip_accents(const std::string& text) {
  // Simplified: just return as-is. Full accent stripping would require
  // Unicode normalization which is complex in C++ without ICU.
  // BERT tokenizer with strip_accents=null defaults to not stripping.
  return text;
}

}  // namespace

bool WordPieceTokenizer::load(const std::filesystem::path& vocab_path) {
  std::ifstream input(vocab_path);
  if (!input) {
    return false;
  }

  vocab_.clear();
  std::string line;
  std::int64_t index = 0;
  while (std::getline(input, line)) {
    // Remove trailing CR if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    vocab_[line] = index;
    if (line == "[CLS]") cls_id_ = index;
    if (line == "[SEP]") sep_id_ = index;
    if (line == "[PAD]") pad_id_ = index;
    if (line == "[UNK]") unk_id_ = index;
    ++index;
  }
  return !vocab_.empty();
}

std::string WordPieceTokenizer::to_lower(const std::string& text) const {
  std::string result;
  result.reserve(text.size());
  for (char c : text) {
    result.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

std::vector<std::string> WordPieceTokenizer::basic_tokenize(
    const std::string& text) const {
  std::vector<std::string> tokens;
  std::string current;

  for (char c : text) {
    if (is_control_char(c)) {
      continue;
    }
    if (is_whitespace(c)) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    if (is_punctuation(c)) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      tokens.push_back(std::string(1, c));
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }

  // Apply lowercasing and accent stripping
  std::vector<std::string> result;
  result.reserve(tokens.size());
  for (auto& token : tokens) {
    token = to_lower(token);
    token = strip_accents(token);
    if (!token.empty()) {
      result.push_back(token);
    }
  }
  return result;
}

std::vector<std::string> WordPieceTokenizer::wordpiece_tokenize(
    const std::string& word) const {
  std::vector<std::string> sub_tokens;
  if (word.size() > 100) {
    // Too long, return [UNK]
    sub_tokens.push_back("[UNK]");
    return sub_tokens;
  }

  const std::string unk_token = "[UNK]";
  constexpr std::size_t max_input_chars_per_word = 200;

  if (word.empty()) return sub_tokens;

  // Greedy longest-match first
  std::size_t start = 0;
  std::vector<std::string> sub_words;
  bool is_bad = false;

  while (start < word.size()) {
    std::size_t end = word.size();
    std::string cur_substr;
    while (start < end) {
      std::string substr = word.substr(start, end - start);
      if (start > 0) {
        substr = "##" + substr;
      }
      if (vocab_.count(substr) > 0) {
        cur_substr = substr;
        break;
      }
      --end;
    }
    if (cur_substr.empty()) {
      is_bad = true;
      break;
    }
    sub_words.push_back(cur_substr);
    start = end;
  }

  if (is_bad) {
    sub_tokens.push_back("[UNK]");
  } else {
    sub_tokens = sub_words;
  }
  return sub_tokens;
}

TokenizedText WordPieceTokenizer::tokenize(
    const std::string& text,
    std::size_t max_length) const {
  TokenizedText result;

  // Basic tokenize -> wordpiece tokenize
  std::vector<std::string> basic_tokens = basic_tokenize(text);
  std::vector<std::string> all_sub_tokens;
  for (const auto& token : basic_tokens) {
    auto sub_tokens = wordpiece_tokenize(token);
    for (const auto& st : sub_tokens) {
      all_sub_tokens.push_back(st);
    }
  }

  // Truncate to max_length - 2 (for [CLS] and [SEP])
  const std::size_t max_content = max_length > 2 ? max_length - 2 : 0;
  if (all_sub_tokens.size() > max_content) {
    all_sub_tokens.resize(max_content);
  }

  // Build input_ids with [CLS] ... [SEP]
  std::vector<std::int64_t> ids;
  ids.push_back(cls_id_);
  for (const auto& token : all_sub_tokens) {
    auto it = vocab_.find(token);
    if (it != vocab_.end()) {
      ids.push_back(it->second);
    } else {
      ids.push_back(unk_id_);
    }
  }
  ids.push_back(sep_id_);

  // No padding — use actual length
  const std::size_t seq_len = ids.size();
  result.input_ids = ids;
  result.token_type_ids = std::vector<std::int64_t>(seq_len, 0);
  result.attention_mask = std::vector<std::int64_t>(seq_len, 1);
  result.seq_len = seq_len;

  return result;
}

}  // namespace svp::vision
