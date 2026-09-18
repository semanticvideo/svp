#include "svp/audio/phoneme_lexicon.hpp"

#include "svp/models/manifest.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace svp::audio {
namespace {

// English MFA phone set (dictionary v3.1.0) to the espeak-ng phone
// inventory emitted by facebook/wav2vec2-lv-60-espeak-cv-ft. Phones not
// listed here map to themselves; most MFA symbols are already espeak.
// Diphthong glide letters differ (MFA j/w vs espeak ɪ/ʊ) and coarticulated
// stops collapse to the nearest espeak realization. "spn" is a non-speech
// token with no espeak equivalent and is dropped from sequences.
const std::unordered_map<std::string, std::string>& mfa_to_espeak_map() {
  static const std::unordered_map<std::string, std::string> map = {
      {"aj", "aɪ"}, {"ej", "eɪ"}, {"ɔj", "ɔɪ"}, {"aw", "aʊ"},
      {"əw", "əʊ"}, {"ow", "oʊ"}, {"cʷ", "kʰ"}, {"kp", "k"},
      {"kʷ", "k"}, {"ɟʷ", "ɟ"}, {"ɡʷ", "ɡ"}, {"pʷ", "p"},
      {"tʷ", "t"}, {"ʈʲ", "ʈ"}, {"ʈʷ", "ʈ"}, {"d̪", "d"},
      {"m̩", "m"}, {"ɒː", "ɒ"}, {"ɝ", "ɜː"}, {"ʉː", "ʉ"},
  };
  return map;
}

// Inflectional suffix pronunciations (espeak phones) applied when the bare
// stem resolves in the dictionary. Ordered longest-first so "ing" wins
// over "s"-style endings where both could match.
const std::vector<std::pair<std::string, std::vector<std::string>>>&
suffix_rules() {
  static const std::vector<std::pair<std::string, std::vector<std::string>>>
      rules = {
          {"ing", {"ɪ", "ŋ"}}, {"est", {"ɪ", "s", "t"}},
          {"ed", {"d"}},        {"es", {"ə", "z"}},
          {"er", {"ɚ"}},       {"ly", {"l", "i"}},
          {"s", {"z"}},        {"'s", {"z"}},
      };
  return rules;
}

std::string fold_curly_apostrophe(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i + 2 < text.size() &&
        static_cast<unsigned char>(text[i]) == 0xE2 &&
        static_cast<unsigned char>(text[i + 1]) == 0x80 &&
        static_cast<unsigned char>(text[i + 2]) == 0x99) {
      result += '\'';
      i += 2;
      continue;
    }
    result += text[i];
  }
  return result;
}

}  // namespace

std::string normalize_word_for_lexicon(const std::string& text) {
  std::string folded = fold_curly_apostrophe(text);
  std::string lowered;
  lowered.reserve(folded.size());
  for (const char c : folded) {
    lowered += static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  }
  // Only alphanumerics can open or close a dictionary word. Apostrophes,
  // hyphens, and non-ASCII bytes are strippable at the edges (whisper emits
  // trailing "--" and unicode dashes) while remaining valid interior
  // characters; interior non-ASCII still fails lookup because the
  // dictionary is ASCII English.
  const auto keep = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c));
  };
  std::size_t begin = 0;
  std::size_t end = lowered.size();
  while (begin < end && !keep(lowered[begin])) ++begin;
  while (end > begin && !keep(lowered[end - 1])) --end;
  return lowered.substr(begin, end - begin);
}

PhonemeLexicon PhonemeLexicon::load(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open pronunciation dictionary: " +
                             path.string());
  }
  PhonemeLexicon lexicon;
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) continue;
    std::string word = line.substr(0, tab);
    std::vector<std::string> phones;
    std::istringstream stream(line.substr(tab + 1));
    std::string token;
    while (stream >> token) {
      const auto mapped = mfa_to_espeak_map().find(token);
      phones.push_back(mapped == mfa_to_espeak_map().end() ? token
                                                         : mapped->second);
    }
    if (!phones.empty()) {
      lexicon.entries_.emplace(std::move(word), std::move(phones));
    }
  }
  if (lexicon.entries_.empty()) {
    throw std::runtime_error("pronunciation dictionary is empty: " +
                             path.string());
  }
  return lexicon;
}

PhonemeLexicon PhonemeLexicon::load_from_bundle(
    const std::filesystem::path& bundle_dir) {
  const std::filesystem::path manifest_path =
      bundle_dir / "model.svpmodel.json";
  if (!std::filesystem::exists(manifest_path)) {
    throw std::runtime_error("aligner bundle manifest not found: " +
                             manifest_path.string());
  }
  const svp::models::ModelBundleManifest manifest =
      svp::models::load_model_bundle_manifest(manifest_path);
  for (const auto& file : manifest.files) {
    if (file.role == "pronunciation_dictionary") {
      return load(bundle_dir / file.path);
    }
  }
  for (const auto& file : manifest.files) {
    if (file.path.size() > 5 &&
        file.path.compare(file.path.size() - 5, 5, ".dict") == 0) {
      return load(bundle_dir / file.path);
    }
  }
  throw std::runtime_error(
      "aligner bundle has no pronunciation_dictionary file: " +
      bundle_dir.string());
}

std::optional<std::vector<std::string>> PhonemeLexicon::phones_for(
    const std::string& normalized_word) const {
  const auto direct = entries_.find(normalized_word);
  if (direct != entries_.end()) {
    std::vector<std::string> phones;
    phones.reserve(direct->second.size());
    for (const std::string& phone : direct->second) {
      if (phone != "spn") phones.push_back(phone);
    }
    if (phones.empty()) return std::nullopt;
    return phones;
  }
  for (const auto& [suffix, suffix_phones] : suffix_rules()) {
    if (normalized_word.size() <= suffix.size() + 1 ||
        normalized_word.compare(normalized_word.size() - suffix.size(),
                                suffix.size(), suffix) != 0) {
      continue;
    }
    const std::string stem =
        normalized_word.substr(0, normalized_word.size() - suffix.size());
    const auto base = entries_.find(stem);
    if (base == entries_.end()) continue;
    std::vector<std::string> phones;
    phones.reserve(base->second.size() + suffix_phones.size());
    for (const std::string& phone : base->second) {
      if (phone != "spn") phones.push_back(phone);
    }
    if (phones.empty()) return std::nullopt;
    phones.insert(phones.end(), suffix_phones.begin(), suffix_phones.end());
    return phones;
  }
  return std::nullopt;
}

}  // namespace svp::audio
