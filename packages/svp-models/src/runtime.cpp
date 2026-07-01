#include "svp/models/runtime.hpp"

#include "svp/models/error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <unistd.h>

#if defined(SVP_ONNX_RUNTIME_AVAILABLE)
#include <onnxruntime_cxx_api.h>
#if defined(SVP_COREML_AVAILABLE)
#include <coreml_provider_factory.h>
#endif
#endif

namespace svp::models {

#if defined(SVP_ONNX_RUNTIME_AVAILABLE)

namespace {
std::atomic<bool> g_onnx_verbose{false};

class StdoutStderrSuppressor {
 public:
  StdoutStderrSuppressor() : suppressed_(false) {
    fflush(stdout);
    fflush(stderr);
    saved_stdout_ = dup(STDOUT_FILENO);
    saved_stderr_ = dup(STDERR_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
      suppressed_ = true;
    }
  }

  ~StdoutStderrSuppressor() {
    if (suppressed_) {
      fflush(stdout);
      fflush(stderr);
      dup2(saved_stdout_, STDOUT_FILENO);
      dup2(saved_stderr_, STDERR_FILENO);
      close(saved_stdout_);
      close(saved_stderr_);
    }
  }

  StdoutStderrSuppressor(const StdoutStderrSuppressor&) = delete;
  StdoutStderrSuppressor& operator=(const StdoutStderrSuppressor&) = delete;

 private:
  bool suppressed_;
  int saved_stdout_;
  int saved_stderr_;
};
}

void set_onnx_verbose(bool verbose) {
  g_onnx_verbose.store(verbose, std::memory_order_relaxed);
}

Ort::Env& shared_onnx_env() {
  const auto level = g_onnx_verbose.load(std::memory_order_relaxed)
      ? ORT_LOGGING_LEVEL_WARNING
      : ORT_LOGGING_LEVEL_FATAL;
  const bool suppress = !g_onnx_verbose.load(std::memory_order_relaxed);
  std::optional<StdoutStderrSuppressor> suppressor;
  if (suppress) {
    suppressor.emplace();
  }
  static Ort::Env env(level, "svp-models");
  return env;
}

struct OnnxSession::Impl {
  Ort::Env* env = &shared_onnx_env();
  Ort::Session session{nullptr};
  std::string model_id_value;
  std::string execution_provider_value;
  std::vector<std::string> input_names;
  std::vector<std::string> output_names;
  std::vector<std::int64_t> input_shape;
  std::vector<std::int64_t> output_shape;
};

OnnxSession::OnnxSession() : impl_(std::make_unique<Impl>()) {}

OnnxSession::~OnnxSession() = default;

OnnxSession::OnnxSession(OnnxSession&&) noexcept = default;

OnnxSession& OnnxSession::operator=(OnnxSession&&) noexcept = default;

bool OnnxSession::is_available() { return true; }

OnnxSession OnnxSession::load(const ModelBundleManifest& manifest,
                              const std::filesystem::path& bundle_root,
                              const OnnxSessionOptions& options) {
  OnnxSession result;
  result.impl_ = std::make_unique<Impl>();
  result.impl_->model_id_value = manifest.model_id;
  result.impl_->execution_provider_value = options.execution_provider;

  std::filesystem::path model_file_path;
  for (const auto& file : manifest.files) {
    if (file.role == "model" || file.role == "onnx" || file.role == "weights") {
      model_file_path = bundle_root / file.path;
      break;
    }
  }
  if (model_file_path.empty()) {
    for (const auto& file : manifest.files) {
      const std::string& p = file.path;
      if (p.size() >= 5 &&
          (p.substr(p.size() - 5) == ".onnx" || p.substr(p.size() - 4) == ".ort")) {
        model_file_path = bundle_root / file.path;
        break;
      }
    }
  }
  if (model_file_path.empty()) {
    throw ModelError(ModelErrorCode::missing_model,
                     "No ONNX model file found in bundle for " + manifest.model_id);
  }
  if (!std::filesystem::exists(model_file_path)) {
    throw ModelError(ModelErrorCode::missing_model,
                     "ONNX model file not found: " + model_file_path.string());
  }

  Ort::SessionOptions session_options;
  if (options.intra_op_num_threads > 0) {
    session_options.SetIntraOpNumThreads(options.intra_op_num_threads);
  }
  if (options.inter_op_num_threads > 0) {
    session_options.SetInterOpNumThreads(options.inter_op_num_threads);
  }

#if defined(SVP_COREML_AVAILABLE)
  if (options.execution_provider == "coreml") {
    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
        session_options, 0));
  }
#else
  if (options.execution_provider == "coreml") {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "CoreML execution provider requested but CoreML support "
                     "is not available in this build");
  }
#endif

  const bool suppress = !g_onnx_verbose.load(std::memory_order_relaxed);
  {
    std::optional<StdoutStderrSuppressor> suppressor;
    if (suppress) {
      suppressor.emplace();
    }
    result.impl_->session = Ort::Session(*result.impl_->env,
                                         model_file_path.string().c_str(),
                                         session_options);
  }

  Ort::AllocatorWithDefaultOptions allocator;

  auto input_count = result.impl_->session.GetInputCount();
  for (std::size_t i = 0; i < input_count; ++i) {
    auto name = result.impl_->session.GetInputNameAllocated(i, allocator);
    result.impl_->input_names.push_back(name.get());
    auto type_info = result.impl_->session.GetInputTypeInfo(i);
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    result.impl_->input_shape = tensor_info.GetShape();
  }

  auto output_count = result.impl_->session.GetOutputCount();
  for (std::size_t i = 0; i < output_count; ++i) {
    auto name = result.impl_->session.GetOutputNameAllocated(i, allocator);
    result.impl_->output_names.push_back(name.get());
    auto type_info = result.impl_->session.GetOutputTypeInfo(i);
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    result.impl_->output_shape = tensor_info.GetShape();
  }

  return result;
}

OnnxIoSpec OnnxSession::io_spec() const {
  OnnxIoSpec spec;
  Ort::AllocatorWithDefaultOptions allocator;

  for (std::size_t i = 0; i < impl_->session.GetInputCount(); ++i) {
    auto name = impl_->session.GetInputNameAllocated(i, allocator);
    auto type_info = impl_->session.GetInputTypeInfo(i);
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    OnnxTensorInfo info;
    info.name = name.get();
    info.shape = tensor_info.GetShape();
    info.dtype = "float32";
    spec.inputs.push_back(std::move(info));
  }

  for (std::size_t i = 0; i < impl_->session.GetOutputCount(); ++i) {
    auto name = impl_->session.GetOutputNameAllocated(i, allocator);
    auto type_info = impl_->session.GetOutputTypeInfo(i);
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    OnnxTensorInfo info;
    info.name = name.get();
    info.shape = tensor_info.GetShape();
    info.dtype = "float32";
    spec.outputs.push_back(std::move(info));
  }

  return spec;
}

std::vector<float> OnnxSession::run_depth(
    const float* input_data,
    std::size_t input_count,
    std::uint32_t width,
    std::uint32_t height) const {
  if (impl_->input_names.empty() || impl_->output_names.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "Session has no input or output names");
  }

  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<std::int64_t> input_shape = impl_->input_shape;
  for (auto& dim : input_shape) {
    if (dim == -1) {
      if (input_shape.size() == 4) {
        if (dim == input_shape[0]) dim = 1;
        else if (dim == input_shape[2]) dim = height;
        else if (dim == input_shape[3]) dim = width;
        else dim = 1;
      } else {
        dim = 1;
      }
    }
  }

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float*>(input_data), input_count,
      input_shape.data(), input_shape.size());

  const char* input_names_cstr = impl_->input_names[0].c_str();
  const char* output_names_cstr = impl_->output_names[0].c_str();

  auto output_tensors = impl_->session.Run(
      Ort::RunOptions{nullptr},
      &input_names_cstr, &input_tensor, 1,
      &output_names_cstr, 1);

  if (output_tensors.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX Runtime produced no output tensors");
  }

  auto& output_tensor = output_tensors[0];
  auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
  auto element_count = type_info.GetElementCount();

  const float* output_data = output_tensor.GetTensorData<float>();
  return std::vector<float>(output_data, output_data + element_count);
}

std::vector<float> OnnxSession::run_embedding(
    const float* input_data,
    std::size_t input_count) const {
  if (impl_->input_names.empty() || impl_->output_names.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "Session has no input or output names");
  }

  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<std::int64_t> input_shape = impl_->input_shape;
  for (auto& dim : input_shape) {
    if (dim == -1) dim = 1;
  }
  if (input_shape.size() == 1) {
    input_shape[0] = static_cast<std::int64_t>(input_count);
  }

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float*>(input_data), input_count,
      input_shape.data(), input_shape.size());

  const char* input_names_cstr = impl_->input_names[0].c_str();
  const char* output_names_cstr = impl_->output_names[0].c_str();

  auto output_tensors = impl_->session.Run(
      Ort::RunOptions{nullptr},
      &input_names_cstr, &input_tensor, 1,
      &output_names_cstr, 1);

  if (output_tensors.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX Runtime produced no output tensors");
  }

  auto& output_tensor = output_tensors[0];
  auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
  auto element_count = type_info.GetElementCount();

  const float* output_data = output_tensor.GetTensorData<float>();
  return std::vector<float>(output_data, output_data + element_count);
}

TextEmbeddingOutput OnnxSession::run_text_embedding(
    const std::int64_t* input_ids,
    const std::int64_t* token_type_ids,
    const std::int64_t* attention_mask,
    std::size_t batch_size,
    std::size_t seq_len) const {
  if (impl_->input_names.size() < 3 || impl_->output_names.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "Text embedding session requires at least 3 inputs and 1 output");
  }

  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);

  std::array<std::int64_t, 2> shape = {
      static_cast<std::int64_t>(batch_size),
      static_cast<std::int64_t>(seq_len)};

  const std::size_t token_count = batch_size * seq_len;

  Ort::Value input_ids_tensor = Ort::Value::CreateTensor<int64_t>(
      memory_info, const_cast<int64_t*>(input_ids), token_count,
      shape.data(), shape.size());
  Ort::Value token_type_tensor = Ort::Value::CreateTensor<int64_t>(
      memory_info, const_cast<int64_t*>(token_type_ids), token_count,
      shape.data(), shape.size());
  Ort::Value attention_mask_tensor = Ort::Value::CreateTensor<int64_t>(
      memory_info, const_cast<int64_t*>(attention_mask), token_count,
      shape.data(), shape.size());

  std::array<Ort::Value, 3> input_tensors = {
      std::move(input_ids_tensor),
      std::move(token_type_tensor),
      std::move(attention_mask_tensor)};

  std::array<const char*, 3> input_names_cstr = {
      impl_->input_names[0].c_str(),
      impl_->input_names[1].c_str(),
      impl_->input_names[2].c_str()};
  const char* output_names_cstr = impl_->output_names[0].c_str();

  auto output_tensors = impl_->session.Run(
      Ort::RunOptions{nullptr},
      input_names_cstr.data(), input_tensors.data(), 3,
      &output_names_cstr, 1);

  if (output_tensors.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX Runtime produced no output tensors");
  }

  auto& output_tensor = output_tensors[0];
  auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
  auto element_count = type_info.GetElementCount();
  auto output_shape = type_info.GetShape();

  const float* output_data = output_tensor.GetTensorData<float>();

  TextEmbeddingOutput result;
  result.data = std::vector<float>(output_data, output_data + element_count);
  result.shape.assign(output_shape.begin(), output_shape.end());
  return result;
}

std::vector<float> OnnxSession::run_visual_embedding(
    const float* input_data,
    std::size_t input_count,
    std::uint32_t width,
    std::uint32_t height) const {
  if (impl_->input_names.empty() || impl_->output_names.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "Session has no input or output names");
  }

  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);

  // Nomic Embed Vision expects NCHW float32 input [1, 3, 224, 224]
  std::vector<std::int64_t> input_shape = {1, 3,
      static_cast<std::int64_t>(height),
      static_cast<std::int64_t>(width)};

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float*>(input_data), input_count,
      input_shape.data(), input_shape.size());

  const char* input_names_cstr = impl_->input_names[0].c_str();
  const char* output_names_cstr = impl_->output_names[0].c_str();

  auto output_tensors = impl_->session.Run(
      Ort::RunOptions{nullptr},
      &input_names_cstr, &input_tensor, 1,
      &output_names_cstr, 1);

  if (output_tensors.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX Runtime produced no output tensors");
  }

  auto& output_tensor = output_tensors[0];
  auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
  auto output_shape = type_info.GetShape();
  auto element_count = type_info.GetElementCount();

  const float* output_data = output_tensor.GetTensorData<float>();

  // Nomic Embed Vision outputs last_hidden_state of shape [batch, seq_len, 768]
  // where seq_len = num_patches + 1 (CLS token at index 0).
  // Extract CLS token (index 0) for a 768-dimensional embedding.
  if (output_shape.size() == 3 && output_shape[2] == 768) {
    return std::vector<float>(output_data, output_data + 768);
  }

  // Fallback: if shape is [batch, 768], return directly
  if (output_shape.size() == 2 && output_shape[1] == 768) {
    return std::vector<float>(output_data, output_data + 768);
  }

  // Unknown shape: return all elements
  return std::vector<float>(output_data, output_data + element_count);
}

std::string OnnxSession::model_id() const {
  return impl_ ? impl_->model_id_value : "";
}

std::string OnnxSession::execution_provider() const {
  return impl_ ? impl_->execution_provider_value : "";
}

std::vector<float> OnnxSession::run_raw(
    const std::string& input_name,
    const float* input_data,
    std::size_t input_count,
    const std::vector<std::int64_t>& input_shape) const {
  if (!impl_ || impl_->session == nullptr) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX session is not loaded");
  }

  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float*>(input_data), input_count,
      input_shape.data(), input_shape.size());

  const char* input_names_cstr = input_name.c_str();
  const char* output_names_cstr = impl_->output_names[0].c_str();

  auto output_tensors = impl_->session.Run(
      Ort::RunOptions{nullptr},
      &input_names_cstr, &input_tensor, 1,
      &output_names_cstr, 1);

  if (output_tensors.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX Runtime produced no output tensors");
  }

  auto& output_tensor = output_tensors[0];
  auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
  auto element_count = type_info.GetElementCount();

  const float* output_data = output_tensor.GetTensorData<float>();
  return std::vector<float>(output_data, output_data + element_count);
}

std::pair<std::vector<float>, std::vector<std::int64_t>>
OnnxSession::run_raw_with_shape(
    const std::string& input_name,
    const float* input_data,
    std::size_t input_count,
    const std::vector<std::int64_t>& input_shape) const {
  if (impl_->input_names.empty() || impl_->output_names.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "Session has no input or output names");
  }

  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float*>(input_data), input_count,
      input_shape.data(), input_shape.size());

  const char* input_names_cstr = input_name.c_str();
  const char* output_names_cstr = impl_->output_names[0].c_str();

  auto output_tensors = impl_->session.Run(
      Ort::RunOptions{nullptr},
      &input_names_cstr, &input_tensor, 1,
      &output_names_cstr, 1);

  if (output_tensors.empty()) {
    throw ModelError(ModelErrorCode::runtime_unavailable,
                     "ONNX Runtime produced no output tensors");
  }

  auto& output_tensor = output_tensors[0];
  auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
  auto element_count = type_info.GetElementCount();
  auto output_shape = type_info.GetShape();

  const float* output_data = output_tensor.GetTensorData<float>();
  return {std::vector<float>(output_data, output_data + element_count),
          output_shape};
}

#else

void set_onnx_verbose(bool) {}

struct OnnxSession::Impl {};

OnnxSession::OnnxSession() : impl_(nullptr) {}
OnnxSession::~OnnxSession() = default;
OnnxSession::OnnxSession(OnnxSession&&) noexcept = default;
OnnxSession& OnnxSession::operator=(OnnxSession&&) noexcept = default;

bool OnnxSession::is_available() {
  return false;
}

OnnxSession OnnxSession::load(const ModelBundleManifest&,
                              const std::filesystem::path&,
                              const OnnxSessionOptions&) {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime support is not configured in this build; model "
                   "bundle parsing and BLAKE3 verification are available");
}

OnnxIoSpec OnnxSession::io_spec() const {
  return {};
}

std::vector<float> OnnxSession::run_depth(const float*, std::size_t,
                                          std::uint32_t, std::uint32_t) const {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime is not available");
}

std::vector<float> OnnxSession::run_raw(
    const std::string&, const float*, std::size_t,
    const std::vector<std::int64_t>&) const {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime is not available");
}

std::pair<std::vector<float>, std::vector<std::int64_t>>
OnnxSession::run_raw_with_shape(
    const std::string&, const float*, std::size_t,
    const std::vector<std::int64_t>&) const {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime is not available");
}

std::vector<float> OnnxSession::run_embedding(const float*, std::size_t) const {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime is not available");
}

TextEmbeddingOutput OnnxSession::run_text_embedding(
    const std::int64_t*, const std::int64_t*, const std::int64_t*,
    std::size_t, std::size_t) const {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime is not available");
}

std::vector<float> OnnxSession::run_visual_embedding(
    const float*, std::size_t, std::uint32_t, std::uint32_t) const {
  throw ModelError(ModelErrorCode::runtime_unavailable,
                   "ONNX Runtime is not available");
}

std::string OnnxSession::model_id() const { return {}; }
std::string OnnxSession::execution_provider() const { return {}; }

#endif

}  // namespace svp::models
