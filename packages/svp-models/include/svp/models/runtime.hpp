#pragma once

#include "svp/models/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace svp::models {

struct OnnxSessionOptions {
  std::string execution_provider = "cpu";
  int intra_op_num_threads = 0;
  int inter_op_num_threads = 0;
};

struct OnnxTensorInfo {
  std::string name;
  std::vector<std::int64_t> shape;
  std::string dtype;
};

struct OnnxIoSpec {
  std::vector<OnnxTensorInfo> inputs;
  std::vector<OnnxTensorInfo> outputs;
};

struct TextEmbeddingOutput {
  std::vector<float> data;
  std::vector<std::int64_t> shape;
};

class OnnxSession {
 public:
  OnnxSession();
  ~OnnxSession();
  OnnxSession(OnnxSession&&) noexcept;
  OnnxSession& operator=(OnnxSession&&) noexcept;
  OnnxSession(const OnnxSession&) = delete;
  OnnxSession& operator=(const OnnxSession&) = delete;

  [[nodiscard]] static bool is_available();

  [[nodiscard]] static OnnxSession load(const ModelBundleManifest& manifest,
                                        const std::filesystem::path& bundle_root,
                                        const OnnxSessionOptions& options);

  [[nodiscard]] OnnxIoSpec io_spec() const;

  [[nodiscard]] std::vector<float> run_depth(
      const float* input_data,
      std::size_t input_count,
      std::uint32_t width,
      std::uint32_t height) const;

  [[nodiscard]] std::vector<float> run_embedding(
      const float* input_data,
      std::size_t input_count) const;

  [[nodiscard]] TextEmbeddingOutput run_text_embedding(
      const std::int64_t* input_ids,
      const std::int64_t* token_type_ids,
      const std::int64_t* attention_mask,
      std::size_t batch_size,
      std::size_t seq_len) const;

  [[nodiscard]] std::vector<float> run_visual_embedding(
      const float* input_data,
      std::size_t input_count,
      std::uint32_t width,
      std::uint32_t height) const;

  [[nodiscard]] std::vector<float> run_raw(
      const std::string& input_name,
      const float* input_data,
      std::size_t input_count,
      const std::vector<std::int64_t>& input_shape) const;

  [[nodiscard]] std::pair<std::vector<float>, std::vector<std::int64_t>>
  run_raw_with_shape(
      const std::string& input_name,
      const float* input_data,
      std::size_t input_count,
      const std::vector<std::int64_t>& input_shape) const;

  [[nodiscard]] std::string model_id() const;
  [[nodiscard]] std::string execution_provider() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace svp::models
