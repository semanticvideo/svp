#include "svp/audio/asr_chunk_planner.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace svp::audio {
namespace {

// Four consecutive normalized tokens are required to anchor a chunk boundary.
// Shorter pairs such as "of the" recur too often to prove shared content.
constexpr std::size_t kMinimumAlignedOverlapTokens = 4;

std::string normalized_token(const std::string& text) {
  std::string token;
  for (const unsigned char character : text) {
    if (std::isalnum(character)) {
      token.push_back(static_cast<char>(std::tolower(character)));
    }
  }
  return token;
}

struct ContiguousMatch {
  std::size_t prior_start = 0;
  std::size_t current_start = 0;
  std::size_t length = 0;
};

ContiguousMatch longest_contiguous_match(
    const std::vector<AsrWord>& prior,
    const std::vector<AsrWord>& current) {
  std::vector<std::size_t> previous(current.size() + 1, 0);
  ContiguousMatch best;
  for (std::size_t i = 1; i <= prior.size(); ++i) {
    std::vector<std::size_t> row(current.size() + 1, 0);
    const std::string prior_token = normalized_token(prior[i - 1].text);
    if (prior_token.empty()) continue;
    for (std::size_t j = 1; j <= current.size(); ++j) {
      if (prior_token != normalized_token(current[j - 1].text)) continue;
      row[j] = previous[j - 1] + 1;
      const ContiguousMatch candidate = {i - row[j], j - row[j], row[j]};
      const std::size_t candidate_current_end =
          candidate.current_start + candidate.length;
      const std::size_t best_current_end = best.current_start + best.length;
      if (candidate.length > best.length ||
          (candidate.length == best.length &&
           candidate_current_end > best_current_end)) {
        best = candidate;
      }
    }
    previous = std::move(row);
  }
  return best;
}

std::vector<AsrWord> globally_timed_words(
    const std::vector<AsrWord>& words,
    const AsrChunkPlan& chunk) {
  std::vector<AsrWord> adjusted;
  for (const AsrWord& word : words) {
    if (word.end_us <= word.start_us) continue;
    AsrWord global = word;
    global.start_us += chunk.source_start_us;
    global.end_us += chunk.source_start_us;
    adjusted.push_back(std::move(global));
  }
  return adjusted;
}

}  // namespace

std::vector<AsrWord> reconcile_overlapping_chunks(
    const std::vector<std::vector<AsrWord>>& chunk_words,
    const std::vector<AsrChunkPlan>& chunks) {
  if (chunk_words.size() != chunks.size()) {
    throw std::invalid_argument(
        "chunk_words size must match chunks size for reconciliation");
  }

  std::vector<AsrWord> result;
  for (std::size_t chunk_index = 0; chunk_index < chunks.size();
       ++chunk_index) {
    std::vector<AsrWord> current =
        globally_timed_words(chunk_words[chunk_index], chunks[chunk_index]);
    if (current.empty()) continue;
    if (result.empty()) {
      result = std::move(current);
      continue;
    }

    const AsrChunkPlan& chunk = chunks[chunk_index];
    const std::int64_t overlap_end_us =
        chunk.source_start_us + chunk.overlap_before_us;
    std::vector<AsrWord> prior_overlap;
    const std::int64_t prior_chunk_ordinal = result.back().chunk_ordinal;
    for (const AsrWord& word : result) {
      if (word.chunk_ordinal == prior_chunk_ordinal) {
        prior_overlap.push_back(word);
      }
    }
    std::vector<AsrWord> current_overlap;
    for (const AsrWord& word : current) {
      if (word.start_us < overlap_end_us) current_overlap.push_back(word);
    }

    const ContiguousMatch match =
        longest_contiguous_match(prior_overlap, current_overlap);
    std::size_t append_from = 0;
    if (match.length >= kMinimumAlignedOverlapTokens) {
      append_from = match.current_start + match.length;
    } else {
      while (append_from < current.size() &&
             current[append_from].start_us < result.back().end_us) {
        ++append_from;
      }
    }
    if (append_from >= current.size()) continue;

    const std::int64_t shift_us = std::max<std::int64_t>(
        0, result.back().end_us - current[append_from].start_us);
    for (std::size_t i = append_from; i < current.size(); ++i) {
      current[i].start_us += shift_us;
      current[i].end_us += shift_us;
      result.push_back(std::move(current[i]));
    }
  }
  return result;
}

}  // namespace svp::audio
