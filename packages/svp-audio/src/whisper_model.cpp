#include "svp/audio/whisper_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

#if defined(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE)
#include <onnxruntime_cxx_api.h>
#endif

namespace svp::audio {
namespace {

#if defined(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE)

constexpr int kSelfCacheDim = 448;
constexpr int kVocabSize = 51864;
constexpr int kMaxDecodeTokens = 224;

struct WhisperModelDims {
  int n_cross_layers = 0;
  int n_self_layers = 0;
  int hidden_dim = 0;
  int vocab_size = 0;
};

struct WhisperSessions {
  std::unique_ptr<Ort::Env> env;
  std::unique_ptr<Ort::Session> encoder;
  std::unique_ptr<Ort::Session> decoder;
  std::vector<std::string> encoder_input_names;
  std::vector<std::string> encoder_output_names;
  std::vector<std::string> decoder_input_names;
  std::vector<std::string> decoder_output_names;
  WhisperModelDims dims;

  explicit WhisperSessions(const std::filesystem::path& model_dir) {
    env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "svp-whisper");

    const std::filesystem::path encoder_path = model_dir / "encoder.int8.onnx";
    const std::filesystem::path decoder_path = model_dir / "decoder.int8.onnx";

    if (!std::filesystem::exists(encoder_path)) {
      throw std::runtime_error("encoder ONNX not found: " + encoder_path.string());
    }
    if (!std::filesystem::exists(decoder_path)) {
      throw std::runtime_error("decoder ONNX not found: " + decoder_path.string());
    }

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    encoder = std::make_unique<Ort::Session>(*env, encoder_path.string().c_str(), opts);
    decoder = std::make_unique<Ort::Session>(*env, decoder_path.string().c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;

    for (std::size_t i = 0; i < encoder->GetInputCount(); ++i) {
      auto name = encoder->GetInputNameAllocated(i, alloc);
      encoder_input_names.push_back(name.get());
    }
    for (std::size_t i = 0; i < encoder->GetOutputCount(); ++i) {
      auto name = encoder->GetOutputNameAllocated(i, alloc);
      encoder_output_names.push_back(name.get());
    }
    for (std::size_t i = 0; i < decoder->GetInputCount(); ++i) {
      auto name = decoder->GetInputNameAllocated(i, alloc);
      decoder_input_names.push_back(name.get());
    }
    for (std::size_t i = 0; i < decoder->GetOutputCount(); ++i) {
      auto name = decoder->GetOutputNameAllocated(i, alloc);
      decoder_output_names.push_back(name.get());
    }

    dims.n_cross_layers = 0;
    dims.n_self_layers = 0;
    dims.hidden_dim = 0;
    dims.vocab_size = kVocabSize;
  }
};

std::vector<float> run_encoder(WhisperSessions& sessions,
                                const std::vector<float>& mel_data,
                                int n_mels, int n_frames,
                                int& out_n_frames) {
  Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  std::array<std::int64_t, 3> mel_shape = {1, n_mels, n_frames};
  Ort::Value mel_tensor = Ort::Value::CreateTensor<float>(
      mem_info, const_cast<float*>(mel_data.data()), mel_data.size(),
      mel_shape.data(), mel_shape.size());

  const char* input_names[] = {sessions.encoder_input_names[0].c_str()};
  const char* output_names[] = {
      sessions.encoder_output_names[0].c_str(),
      sessions.encoder_output_names[1].c_str(),
  };

  auto outputs = sessions.encoder->Run(
      Ort::RunOptions{nullptr},
      input_names, &mel_tensor, 1,
      output_names, 2);

  if (outputs.size() != 2) {
    throw std::runtime_error("encoder produced unexpected output count");
  }

  auto k_info = outputs[0].GetTensorTypeAndShapeInfo();
  auto k_shape = k_info.GetShape();
  const std::size_t k_count = k_info.GetElementCount();
  const std::size_t v_count = outputs[1].GetTensorTypeAndShapeInfo().GetElementCount();

  out_n_frames = static_cast<int>(k_shape[2]);

  if (sessions.dims.n_cross_layers == 0) {
    sessions.dims.n_cross_layers = static_cast<int>(k_shape[0]);
    sessions.dims.hidden_dim = static_cast<int>(k_shape[3]);
    sessions.dims.n_self_layers = static_cast<int>(k_shape[0]);
  }

  const float* k_data = outputs[0].GetTensorData<float>();
  const float* v_data = outputs[1].GetTensorData<float>();

  std::vector<float> cross_kv(k_count + v_count);
  std::memcpy(cross_kv.data(), k_data, k_count * sizeof(float));
  std::memcpy(cross_kv.data() + k_count, v_data, v_count * sizeof(float));

  return cross_kv;
}

struct DecoderKVCache {
  std::vector<float> self_k;
  std::vector<float> self_v;
  std::vector<float> cross_k;
  std::vector<float> cross_v;
  std::int64_t offset = 0;
};

std::vector<float> run_decoder_step(
    WhisperSessions& sessions,
    const std::vector<std::int32_t>& tokens,
    DecoderKVCache& cache,
    int n_frames) {
  Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  const auto& d = sessions.dims;
  const std::int64_t n_tokens = static_cast<std::int64_t>(tokens.size());

  std::array<std::int64_t, 2> token_shape = {1, n_tokens};
  std::vector<std::int64_t> tokens_i64(tokens.begin(), tokens.end());
  Ort::Value token_tensor = Ort::Value::CreateTensor<std::int64_t>(
      mem_info, tokens_i64.data(), tokens_i64.size(),
      token_shape.data(), token_shape.size());

  std::array<std::int64_t, 4> self_k_shape = {d.n_self_layers, 1, kSelfCacheDim, d.hidden_dim};
  Ort::Value self_k_tensor = Ort::Value::CreateTensor<float>(
      mem_info, cache.self_k.data(), cache.self_k.size(),
      self_k_shape.data(), self_k_shape.size());

  std::array<std::int64_t, 4> self_v_shape = {d.n_self_layers, 1, kSelfCacheDim, d.hidden_dim};
  Ort::Value self_v_tensor = Ort::Value::CreateTensor<float>(
      mem_info, cache.self_v.data(), cache.self_v.size(),
      self_v_shape.data(), self_v_shape.size());

  std::array<std::int64_t, 4> cross_k_shape = {d.n_cross_layers, 1, n_frames, d.hidden_dim};
  Ort::Value cross_k_tensor = Ort::Value::CreateTensor<float>(
      mem_info, cache.cross_k.data(), cache.cross_k.size(),
      cross_k_shape.data(), cross_k_shape.size());

  std::array<std::int64_t, 4> cross_v_shape = {d.n_cross_layers, 1, n_frames, d.hidden_dim};
  Ort::Value cross_v_tensor = Ort::Value::CreateTensor<float>(
      mem_info, cache.cross_v.data(), cache.cross_v.size(),
      cross_v_shape.data(), cross_v_shape.size());

  std::array<std::int64_t, 1> offset_shape = {1};
  Ort::Value offset_tensor = Ort::Value::CreateTensor<std::int64_t>(
      mem_info, &cache.offset, 1,
      offset_shape.data(), offset_shape.size());

  std::array<Ort::Value, 6> inputs = {
      std::move(token_tensor),
      std::move(self_k_tensor),
      std::move(self_v_tensor),
      std::move(cross_k_tensor),
      std::move(cross_v_tensor),
      std::move(offset_tensor),
  };

  std::array<const char*, 6> input_names = {
      sessions.decoder_input_names[0].c_str(),
      sessions.decoder_input_names[1].c_str(),
      sessions.decoder_input_names[2].c_str(),
      sessions.decoder_input_names[3].c_str(),
      sessions.decoder_input_names[4].c_str(),
      sessions.decoder_input_names[5].c_str(),
  };

  std::array<const char*, 3> output_names = {
      sessions.decoder_output_names[0].c_str(),
      sessions.decoder_output_names[1].c_str(),
      sessions.decoder_output_names[2].c_str(),
  };

  auto outputs = sessions.decoder->Run(
      Ort::RunOptions{nullptr},
      input_names.data(), inputs.data(), 6,
      output_names.data(), 3);

  if (outputs.size() != 3) {
    throw std::runtime_error("decoder produced unexpected output count");
  }

  auto logits_info = outputs[0].GetTensorTypeAndShapeInfo();
  auto logits_shape = logits_info.GetShape();
  const float* logits_data = outputs[0].GetTensorData<float>();

  const std::size_t n_steps = static_cast<std::size_t>(logits_shape[1]);
  const std::size_t last_step_offset = (n_steps - 1) * static_cast<std::size_t>(d.vocab_size);

  std::vector<float> last_logits(static_cast<std::size_t>(d.vocab_size));
  std::memcpy(last_logits.data(), logits_data + last_step_offset,
              static_cast<std::size_t>(d.vocab_size) * sizeof(float));

  auto out_k_info = outputs[1].GetTensorTypeAndShapeInfo();
  auto out_v_info = outputs[2].GetTensorTypeAndShapeInfo();
  const std::size_t out_k_count = out_k_info.GetElementCount();
  const std::size_t out_v_count = out_v_info.GetElementCount();

  const float* out_k_data = outputs[1].GetTensorData<float>();
  const float* out_v_data = outputs[2].GetTensorData<float>();

  cache.self_k.assign(out_k_data, out_k_data + out_k_count);
  cache.self_v.assign(out_v_data, out_v_data + out_v_count);
  cache.offset += static_cast<std::int64_t>(n_steps);

  return last_logits;
}

int argmax(const std::vector<float>& logits) {
  int best = 0;
  float best_val = logits[0];
  for (int i = 1; i < static_cast<int>(logits.size()); ++i) {
    if (logits[i] > best_val) {
      best_val = logits[i];
      best = i;
    }
  }
  return best;
}

std::string decode_tokens_to_text(const std::vector<int>& token_ids,
                                   const WhisperTokenTable& token_table) {
  std::string text;
  for (int id : token_ids) {
    if (id == WhisperTokenTable::kEot) continue;
    if (id == WhisperTokenTable::kSot) continue;
    if (id >= 50357) continue;

    auto token_str = token_table.token_text(id);
    if (!token_str) continue;

    std::string t = *token_str;
    if (!t.empty() && t[0] == ' ' && !text.empty()) {
      text += ' ';
      text += t.substr(1);
    } else {
      text += t;
    }
  }
  return text;
}

std::vector<AsrWord> decode_tokens_to_words(const std::vector<int>& token_ids,
                                             const WhisperTokenTable& token_table,
                                             std::int64_t chunk_start_us,
                                             std::int64_t chunk_end_us) {
  constexpr int kTimestampBase = 50357;
  constexpr double kTimestampIntervalUs = 20000.0;

  struct TimestampSegment {
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
    std::vector<int> text_token_ids;
  };

  std::vector<TimestampSegment> segments;
  TimestampSegment current;
  current.start_us = chunk_start_us;
  current.end_us = chunk_end_us;
  bool in_segment = false;

  for (int id : token_ids) {
    if (id == WhisperTokenTable::kEot || id == WhisperTokenTable::kSot) continue;

    if (id >= kTimestampBase) {
      std::int64_t ts_us = static_cast<std::int64_t>(
          static_cast<double>(id - kTimestampBase) * kTimestampIntervalUs) + chunk_start_us;

      if (!in_segment) {
        current.start_us = ts_us;
        current.text_token_ids.clear();
        in_segment = true;
      } else {
        current.end_us = ts_us;
        if (!current.text_token_ids.empty()) {
          segments.push_back(current);
        }
        current = TimestampSegment{};
        current.start_us = ts_us;
        in_segment = true;
      }
      continue;
    }

    if (in_segment) {
      current.text_token_ids.push_back(id);
    } else {
      current.text_token_ids.push_back(id);
    }
  }

  if (in_segment && !current.text_token_ids.empty()) {
    current.end_us = chunk_end_us;
    segments.push_back(current);
  }

  if (segments.empty()) {
    std::string text = decode_tokens_to_text(token_ids, token_table);
    if (text.empty()) return {};

    std::vector<AsrWord> words;
    std::istringstream iss(text);
    std::string w;
    std::int64_t chunk_dur = chunk_end_us - chunk_start_us;
    std::vector<std::string> all_words;
    while (iss >> w) all_words.push_back(w);
    if (all_words.empty()) return {};

    std::int64_t per_word = chunk_dur / static_cast<std::int64_t>(all_words.size());
    for (std::size_t i = 0; i < all_words.size(); ++i) {
      AsrWord word;
      word.text = all_words[i];
      word.start_us = chunk_start_us + static_cast<std::int64_t>(i) * per_word;
      word.end_us = (i + 1 == all_words.size())
          ? chunk_end_us
          : chunk_start_us + static_cast<std::int64_t>(i + 1) * per_word;
      word.confidence = 0.0;
      word.chunk_ordinal = 0;
      words.push_back(word);
    }
    return words;
  }

  std::vector<AsrWord> words;
  for (const auto& seg : segments) {
    std::string seg_text;
    for (int id : seg.text_token_ids) {
      auto token_str = token_table.token_text(id);
      if (!token_str) continue;
      std::string t = *token_str;
      if (!t.empty() && t[0] == ' ' && !seg_text.empty()) {
        seg_text += ' ';
        seg_text += t.substr(1);
      } else {
        seg_text += t;
      }
    }

    if (seg_text.empty()) continue;

    std::istringstream iss(seg_text);
    std::string w;
    std::vector<std::string> seg_words;
    while (iss >> w) seg_words.push_back(w);
    if (seg_words.empty()) continue;

    std::int64_t seg_dur = seg.end_us - seg.start_us;
    std::int64_t per_word = seg_dur / static_cast<std::int64_t>(seg_words.size());
    for (std::size_t i = 0; i < seg_words.size(); ++i) {
      AsrWord word;
      word.text = seg_words[i];
      word.start_us = seg.start_us + static_cast<std::int64_t>(i) * per_word;
      word.end_us = (i + 1 == seg_words.size())
          ? seg.end_us
          : seg.start_us + static_cast<std::int64_t>(i + 1) * per_word;
      word.confidence = 0.0;
      word.chunk_ordinal = 0;
      words.push_back(word);
    }
  }

  return words;
}

WhisperInferenceResult run_whisper_internal(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::string& chunk_id,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us) {
  WhisperInferenceResult result;
  result.ran = false;

  WhisperTokenTable token_table;
  const std::filesystem::path tokens_path = model_dir / "tokens.txt";
  if (!token_table.load(tokens_path)) {
    result.blockers.push_back("failed to load Whisper token table");
    return result;
  }

  WhisperMelFeatures mel = compute_whisper_mel_from_wav(wav_path);

  WhisperSessions sessions(model_dir);

  int enc_n_frames = 0;
  std::vector<float> cross_kv = run_encoder(sessions, mel.data, mel.n_mels, mel.n_frames, enc_n_frames);

  const auto& d = sessions.dims;
  const std::size_t cross_kv_per = static_cast<std::size_t>(d.n_cross_layers) * 1 * static_cast<std::size_t>(enc_n_frames) * static_cast<std::size_t>(d.hidden_dim);
  DecoderKVCache cache;
  cache.cross_k.assign(cross_kv.begin(), cross_kv.begin() + cross_kv_per);
  cache.cross_v.assign(cross_kv.begin() + cross_kv_per, cross_kv.end());
  cache.self_k.assign(static_cast<std::size_t>(d.n_self_layers) * 1 * kSelfCacheDim * static_cast<std::size_t>(d.hidden_dim), 0.0f);
  cache.self_v.assign(static_cast<std::size_t>(d.n_self_layers) * 1 * kSelfCacheDim * static_cast<std::size_t>(d.hidden_dim), 0.0f);
  cache.offset = 0;

  std::vector<std::int32_t> tokens = {WhisperTokenTable::kSot};
  std::vector<int> decoded_token_ids;

  for (int step = 0; step < kMaxDecodeTokens; ++step) {
    std::vector<float> logits = run_decoder_step(sessions, tokens, cache, enc_n_frames);

    int next_token = argmax(logits);
    if (next_token == WhisperTokenTable::kEot) break;

    decoded_token_ids.push_back(next_token);
    tokens = {next_token};
  }

  std::string text = decode_tokens_to_text(decoded_token_ids, token_table);

  if (!text.empty()) {
    WhisperSegment segment;
    segment.text = text;
    segment.start_us = chunk_start_us;
    segment.end_us = chunk_end_us;

    segment.words = decode_tokens_to_words(decoded_token_ids, token_table,
                                            chunk_start_us, chunk_end_us);
    result.all_words = segment.words;

    result.segments.push_back(std::move(segment));
  }

  result.ran = true;
  return result;
}

#endif  // SVP_AUDIO_ONNX_RUNTIME_AVAILABLE

}  // namespace

bool is_whisper_runtime_available() {
#if defined(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE)
  return true;
#else
  return false;
#endif
}

WhisperInferenceResult run_whisper_inference(
    const std::filesystem::path& wav_path,
    const std::filesystem::path& model_dir,
    const std::string& chunk_id,
    std::int64_t chunk_start_us,
    std::int64_t chunk_end_us) {
#if defined(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE)
  try {
    return run_whisper_internal(wav_path, model_dir, chunk_id,
                                 chunk_start_us, chunk_end_us);
  } catch (const std::exception& e) {
    WhisperInferenceResult result;
    result.ran = false;
    result.blockers.push_back(std::string("Whisper inference failed: ") + e.what());
    return result;
  }
#else
  WhisperInferenceResult result;
  result.ran = false;
  result.blockers.push_back("ONNX Runtime is not available for Whisper inference");
  return result;
#endif
}

}  // namespace svp::audio
