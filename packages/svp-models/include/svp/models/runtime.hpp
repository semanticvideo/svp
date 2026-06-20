#pragma once

#include "svp/models/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
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

  [[nodiscard]] std::string model_id() const;
  [[nodiscard]] std::string execution_provider() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace svp::models
