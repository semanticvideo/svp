#include "svp/audio/ctc_forced_aligner.hpp"

#include "svp/models/manifest.hpp"
#include "svp/models/runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace svp::audio {
namespace {

// wav2vec2 emits one logit frame per ~320 input samples (conv strides
// 5*2*2*2*2). Token times use the torchaudio forced-alignment convention
// num_samples / num_frames, which absorbs the conv receptive-field offset.
constexpr double kSampleRate = 16000.0;
// The wav2vec2 feature extractor convolves with a 320-sample stride, so each
// emission frame advances exactly 20 ms of audio regardless of clip length.
constexpr double kFrameSeconds = 320.0 / kSampleRate;
constexpr double kNormalizationEpsilon = 1e-7;
constexpr double kNegInf = -1e30;

std::filesystem::path manifest_file(const svp::models::ModelBundleManifest& manifest,
                                    const std::filesystem::path& bundle_dir,
                                    const std::string& role,
                                    const char* extension) {
  for (const auto& file : manifest.files) {
    if (file.role == role) return bundle_dir / file.path;
  }
  for (const auto& file : manifest.files) {
    const std::string& path = file.path;
    if (path.size() > std::string(extension).size() &&
        path.compare(path.size() - std::string(extension).size(),
                     std::string(extension).size(), extension) == 0) {
      return bundle_dir / path;
    }
  }
  throw std::runtime_error(
      "aligner bundle has no file with role " + role + " in " +
      bundle_dir.string());
}

std::vector<float> normalize_samples(const std::vector<float>& samples) {
  const double count = static_cast<double>(samples.size());
  double mean = 0.0;
  for (const float sample : samples) mean += sample;
  mean /= count;
  double variance = 0.0;
  for (const float sample : samples) {
    const double centered = sample - mean;
    variance += centered * centered;
  }
  variance /= count;
  const double scale = 1.0 / std::sqrt(variance + kNormalizationEpsilon);
  std::vector<float> normalized(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    normalized[i] = static_cast<float>((samples[i] - mean) * scale);
  }
  return normalized;
}

}  // namespace

struct CtcForcedAligner::Impl {
  svp::models::OnnxSession session;
  std::vector<std::string> id_to_phone;
  int blank_id = 0;
};

CtcForcedAligner::CtcForcedAligner(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

CtcForcedAligner CtcForcedAligner::load(
    const std::filesystem::path& bundle_dir) {
  const std::filesystem::path manifest_path =
      bundle_dir / "model.svpmodel.json";
  if (!std::filesystem::exists(manifest_path)) {
    throw std::runtime_error("aligner bundle manifest not found: " +
                             manifest_path.string());
  }
  const svp::models::ModelBundleManifest manifest =
      svp::models::load_model_bundle_manifest(manifest_path);

  auto impl = std::make_shared<Impl>();
  svp::models::OnnxSessionOptions options;
  options.execution_provider = "cpu";
  impl->session = svp::models::OnnxSession::load(manifest, bundle_dir, options);

  const std::filesystem::path vocab_path =
      manifest_file(manifest, bundle_dir, "vocabulary", ".json");
  const nlohmann::json vocab = nlohmann::json::parse(
      std::ifstream(vocab_path));
  for (const auto& [phone, id] : vocab.items()) {
    const int index = id.get<int>();
    if (index >= static_cast<int>(impl->id_to_phone.size())) {
      impl->id_to_phone.resize(static_cast<std::size_t>(index) + 1);
    }
    impl->id_to_phone[static_cast<std::size_t>(index)] = phone;
    if (phone == "<pad>") impl->blank_id = index;
  }
  if (impl->id_to_phone.empty()) {
    throw std::runtime_error("aligner vocabulary is empty: " +
                             vocab_path.string());
  }
  return CtcForcedAligner(std::move(impl));
}

std::vector<CtcPhoneSpan> CtcForcedAligner::align(
    const std::vector<float>& samples,
    const std::vector<CtcAlignmentToken>& tokens) const {
  if (tokens.empty()) return {};
  const std::vector<float> normalized = normalize_samples(samples);

  const auto [logits, shape] = impl_->session.run_raw_with_shape(
      "input_values", normalized.data(), normalized.size(),
      {1, static_cast<std::int64_t>(normalized.size())});
  if (shape.size() != 3 || shape[0] != 1) {
    throw std::runtime_error("aligner returned unexpected logits shape");
  }
  const std::size_t frames = static_cast<std::size_t>(shape[1]);
  const std::size_t vocab_size = static_cast<std::size_t>(shape[2]);

  std::unordered_map<std::string, int> token_ids;
  for (std::size_t id = 0; id < impl_->id_to_phone.size(); ++id) {
    if (!impl_->id_to_phone[id].empty()) {
      token_ids.emplace(impl_->id_to_phone[id], static_cast<int>(id));
    }
  }
  std::vector<int> sequence;
  sequence.reserve(tokens.size());
  for (const CtcAlignmentToken& token : tokens) {
    const auto it = token_ids.find(token.phone);
    if (it == token_ids.end()) {
      throw std::runtime_error("phone missing from aligner vocabulary: " +
                               token.phone);
    }
    sequence.push_back(it->second);
  }

  // CTC path expansion: a blank before and after every token. Transition
  // rules mirror torchaudio forced alignment: stay, advance by one, or
  // skip a blank/duplicate by two.
  const std::size_t expanded = sequence.size() * 2 + 1;
  std::vector<int> states(expanded);
  std::vector<int> state_token(expanded, -1);
  for (std::size_t i = 0; i < sequence.size(); ++i) {
    states[i * 2] = impl_->blank_id;
    states[i * 2 + 1] = sequence[i];
    state_token[i * 2 + 1] = static_cast<int>(i);
  }
  states[expanded - 1] = impl_->blank_id;

  // log_softmax over each frame's logits in float32, matching the
  // reference implementation's accumulation precision.
  std::vector<float> emissions(frames * vocab_size);
  for (std::size_t f = 0; f < frames; ++f) {
    const float* row = logits.data() + f * vocab_size;
    const float max_logit = *std::max_element(row, row + vocab_size);
    float sum = 0.0F;
    for (std::size_t v = 0; v < vocab_size; ++v) {
      sum += std::exp(row[v] - max_logit);
    }
    const float log_sum = std::log(sum);
    for (std::size_t v = 0; v < vocab_size; ++v) {
      emissions[f * vocab_size + v] = row[v] - max_logit - log_sum;
    }
  }

  std::vector<float> trellis(frames * expanded,
                             static_cast<float>(kNegInf));
  std::vector<int> backpointers(frames * expanded, 0);
  trellis[0] = emissions[states[0]];
  if (expanded > 1) trellis[1] = emissions[states[1]];
  for (std::size_t f = 1; f < frames; ++f) {
    for (std::size_t s = 0; s < expanded; ++s) {
      float best = trellis[(f - 1) * expanded + s];
      int choice = 0;
      if (s > 0 && trellis[(f - 1) * expanded + s - 1] > best) {
        best = trellis[(f - 1) * expanded + s - 1];
        choice = 1;
      }
      if (s > 1 && states[s] != impl_->blank_id &&
          states[s] != states[s - 2] &&
          trellis[(f - 1) * expanded + s - 2] > best) {
        best = trellis[(f - 1) * expanded + s - 2];
        choice = 2;
      }
      trellis[f * expanded + s] =
          best + emissions[f * vocab_size + states[s]];
      backpointers[f * expanded + s] = choice;
    }
  }

  std::size_t state = expanded - 1;
  if (expanded > 1 &&
      trellis[(frames - 1) * expanded + expanded - 2] >
          trellis[(frames - 1) * expanded + expanded - 1]) {
    state = expanded - 2;
  }
  std::vector<std::size_t> path(frames);
  for (std::size_t f = frames; f-- > 0;) {
    path[f] = state;
    const int choice = backpointers[f * expanded + state];
    if (choice == 2) {
      state -= 2;
    } else if (choice == 1) {
      state -= 1;
    }
  }

  std::vector<CtcPhoneSpan> spans;
  spans.reserve(sequence.size());
  std::size_t frame = 0;
  while (frame < frames) {
    const int token = state_token[path[frame]];
    if (token < 0) {
      ++frame;
      continue;
    }
    const std::size_t first = frame;
    while (frame < frames && state_token[path[frame]] == token) ++frame;
    CtcPhoneSpan span;
    span.token_index = static_cast<std::size_t>(token);
    span.start_seconds = first * kFrameSeconds;
    span.end_seconds = static_cast<double>(frame) * kFrameSeconds;
    spans.push_back(span);
  }
  return spans;
}

}  // namespace svp::audio
