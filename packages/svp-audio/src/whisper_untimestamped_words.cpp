#include "svp/audio/whisper_untimestamped_words.hpp"

#include "svp/audio/whisper_model.hpp"

#include <string>
#include <utility>

namespace svp::audio {
namespace {

struct WordTokens {
  std::string text;
  std::vector<std::size_t> token_indices;
};

}  // namespace

std::vector<AsrWord> decode_untimestamped_whisper_words(
    const std::vector<int>& token_ids,
    const std::vector<double>& token_probabilities,
    const WhisperTokenTable& token_table,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us,
    int timestamp_begin) {
  std::vector<WordTokens> groups;
  WordTokens current;
  for (std::size_t index = 0; index < token_ids.size(); ++index) {
    const int token_id = token_ids[index];
    if (token_id >= timestamp_begin ||
        token_id == WhisperTokenTable::kEot ||
        token_id == WhisperTokenTable::kSot) {
      continue;
    }
    const auto token = token_table.token_text(token_id);
    if (!token.has_value()) continue;
    if (!token->empty() && token->front() == ' ') {
      if (!current.text.empty()) {
        groups.push_back(std::move(current));
        current = WordTokens{};
      }
      current.text = token->substr(1);
    } else {
      current.text += *token;
    }
    current.token_indices.push_back(index);
  }
  if (!current.text.empty()) groups.push_back(std::move(current));
  if (groups.empty()) return {};

  const std::int64_t duration_us = chunk_end_us - chunk_start_us;
  const std::int64_t per_word_us =
      duration_us / static_cast<std::int64_t>(groups.size());
  std::vector<AsrWord> words;
  words.reserve(groups.size());
  for (std::size_t index = 0; index < groups.size(); ++index) {
    AsrWord word;
    word.text = groups[index].text;
    word.start_us =
        chunk_start_us + static_cast<std::int64_t>(index) * per_word_us;
    word.end_us = index + 1 == groups.size()
                      ? chunk_end_us
                      : chunk_start_us +
                            static_cast<std::int64_t>(index + 1) * per_word_us;
    word.confidence = aggregate_word_confidence(
        token_probabilities, groups[index].token_indices);
    words.push_back(std::move(word));
  }
  return words;
}

}  // namespace svp::audio
