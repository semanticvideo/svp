#include "svp/vision/text_embedding_work.hpp"

#include <cmath>
#include <exception>

namespace svp::vision {
namespace {

std::vector<float> mean_pool_and_normalize(
    const std::vector<float>& last_hidden_state,
    const std::vector<std::int64_t>& attention_mask,
    std::size_t seq_len,
    std::uint32_t embedding_dim) {
  std::vector<float> pooled(embedding_dim, 0.0f);
  float mask_sum = 0.0f;

  for (std::size_t t = 0; t < seq_len; ++t) {
    if (attention_mask[t] == 0) continue;
    mask_sum += 1.0f;
    for (std::uint32_t d = 0; d < embedding_dim; ++d) {
      pooled[d] += last_hidden_state[t * embedding_dim + d];
    }
  }

  if (mask_sum > 0.0f) {
    const float inv = 1.0f / mask_sum;
    for (std::uint32_t d = 0; d < embedding_dim; ++d) {
      pooled[d] *= inv;
    }
  }

  float norm = 0.0f;
  for (std::uint32_t d = 0; d < embedding_dim; ++d) {
    norm += pooled[d] * pooled[d];
  }
  norm = std::sqrt(norm);
  if (norm > 1e-12f) {
    const float inv_norm = 1.0f / norm;
    for (std::uint32_t d = 0; d < embedding_dim; ++d) {
      pooled[d] *= inv_norm;
    }
  }

  return pooled;
}

TextEmbeddingOutcome failed(std::string blocker) {
  TextEmbeddingOutcome outcome;
  outcome.error = std::move(blocker);
  return outcome;
}

}  // namespace

bool embedding_vector_is_valid(const std::vector<float>& embedding,
                               std::uint32_t expected_dim) {
  if (embedding.size() != expected_dim) return false;
  bool has_nan = false;
  bool has_inf = false;
  float norm_sq = 0.0f;
  for (std::uint32_t d = 0; d < expected_dim; ++d) {
    if (std::isnan(embedding[d])) has_nan = true;
    if (std::isinf(embedding[d])) has_inf = true;
    norm_sq += embedding[d] * embedding[d];
  }
  if (has_nan || has_inf) return false;
  const float norm = std::sqrt(norm_sq);
  if (norm < 0.9f || norm > 1.1f) return false;
  if (norm_sq < 1e-20f) return false;
  return true;
}

TextEmbeddingOutcome embed_text_item(const svp::models::OnnxSession& session,
                                     const WordPieceTokenizer& tokenizer,
                                     const TextEmbeddingItem& item,
                                     std::uint32_t embedding_dim) {
  TokenizedText tokenized = tokenizer.tokenize(item.text, kTextEmbeddingMaxTokens);

  svp::models::TextEmbeddingOutput embedding_output;
  try {
    embedding_output = session.run_text_embedding(
        tokenized.input_ids.data(),
        tokenized.token_type_ids.data(),
        tokenized.attention_mask.data(),
        1,
        tokenized.seq_len);
  } catch (const std::exception& e) {
    return failed(std::string("ONNX text embedding inference failed for ") +
                  item.id + ": " + e.what());
  }

  if (embedding_output.shape.size() != 3) {
    return failed(std::string("ONNX output shape validation failed for ") +
                  item.id + ": expected rank-3 output, got rank " +
                  std::to_string(embedding_output.shape.size()));
  }

  for (std::size_t i = 0; i < 3; ++i) {
    if (embedding_output.shape[i] <= 0) {
      return failed(std::string("ONNX output shape validation failed for ") +
                    item.id + ": dimension " + std::to_string(i) +
                    " is not positive, got " +
                    std::to_string(embedding_output.shape[i]));
    }
  }

  if (embedding_output.shape[0] != 1) {
    return failed(std::string("ONNX output batch mismatch for ") +
                  item.id + ": expected 1, got " +
                  std::to_string(embedding_output.shape[0]));
  }

  const std::size_t out_batch =
      static_cast<std::size_t>(embedding_output.shape[0]);
  const std::size_t out_seq_len =
      static_cast<std::size_t>(embedding_output.shape[1]);
  const std::size_t out_dim =
      static_cast<std::size_t>(embedding_output.shape[2]);

  const std::size_t expected_elements = out_batch * out_seq_len * out_dim;
  if (embedding_output.data.size() != expected_elements) {
    return failed(std::string("ONNX output element count mismatch for ") +
                  item.id + ": shape implies " +
                  std::to_string(expected_elements) + " elements, got " +
                  std::to_string(embedding_output.data.size()));
  }

  if (out_dim != embedding_dim) {
    return failed(std::string("ONNX output dimension mismatch for ") +
                  item.id + ": expected " +
                  std::to_string(embedding_dim) + ", got " +
                  std::to_string(out_dim));
  }

  if (out_seq_len != tokenized.seq_len) {
    return failed(std::string("ONNX output seq_len mismatch for ") +
                  item.id + ": expected " +
                  std::to_string(tokenized.seq_len) + ", got " +
                  std::to_string(out_seq_len));
  }

  std::vector<float> embedding = mean_pool_and_normalize(
      embedding_output.data, tokenized.attention_mask,
      tokenized.seq_len, embedding_dim);

  if (!embedding_vector_is_valid(embedding, embedding_dim)) {
    return failed(std::string("Embedding validation failed for ") +
                  item.id + ": invalid vector (NaN, Inf, zero, or wrong dimension)");
  }

  TextEmbeddingOutcome outcome;
  outcome.vector = std::move(embedding);
  return outcome;
}

}  // namespace svp::vision
