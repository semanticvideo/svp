#pragma once

// Per-observation work of the text-embedding stage (dispatched_work.hpp):
// tokenize one observation's text, run the text model, mean-pool and
// L2-normalize, and check the vector. generate_embedding_blocks runs exactly
// this function for each observation, so a vector computed by an
// embed.text_batch task is the vector the stage itself would have written.

#include "svp/models/runtime.hpp"
#include "svp/vision/dispatched_work.hpp"
#include "svp/vision/text_tokenizer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace svp::vision {

// Longest token sequence the text model is given (the stage's tokenizer
// bound; nomic-embed-text v1.5 takes up to 512 positions in its BERT
// encoder, the standard WordPiece limit).
inline constexpr std::size_t kTextEmbeddingMaxTokens = 512;

struct TextEmbeddingItem {
  // The observation id, named in a failure's blocker text.
  std::string id;
  std::string text;

  bool operator==(const TextEmbeddingItem&) const = default;
};

struct TextEmbeddingOutcome {
  // embedding_dim floats when `error` is empty.
  std::vector<float> vector;
  // The stage's blocker text for this observation when it failed.
  std::string error;

  bool operator==(const TextEmbeddingOutcome&) const = default;
};

// One observation, as the stage embeds it.
[[nodiscard]] TextEmbeddingOutcome embed_text_item(const svp::models::OnnxSession& session,
                                                   const WordPieceTokenizer& tokenizer,
                                                   const TextEmbeddingItem& item,
                                                   std::uint32_t embedding_dim);

// The stage's acceptance check of any embedding vector: the expected
// dimension, finite values, and an L2 norm within 10% of 1 (vectors are
// L2-normalized, so anything else means the model output was not usable).
[[nodiscard]] bool embedding_vector_is_valid(const std::vector<float>& embedding,
                                             std::uint32_t expected_dim);

// Embeds every item with `model` into `embedding_dim` values and returns one
// outcome per item, in item order; nullopt when the items must be embedded in
// the stage itself. `on_progress(done, total)` is called as outcomes arrive.
// Throws DispatchedWorkError when it cannot deliver.
using TextEmbeddingDispatcher =
    std::function<std::optional<std::vector<TextEmbeddingOutcome>>(
        const std::vector<TextEmbeddingItem>& items, const DispatchedModel& model,
        std::uint32_t embedding_dim,
        const std::function<void(std::size_t done, std::size_t total)>& on_progress)>;

}  // namespace svp::vision
