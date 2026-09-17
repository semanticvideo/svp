#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace svp::audio {

// Normalizes an orthographic token for lexicon lookup: lowercases ASCII,
// folds curly apostrophes, and strips leading/trailing punctuation while
// keeping interior apostrophes and hyphens.
[[nodiscard]] std::string normalize_word_for_lexicon(const std::string& text);

// Pronunciation lexicon backed by the English MFA dictionary (MFA phone
// set). Lookup output is translated to the espeak-ng phone inventory used
// by the wav2vec2 espeak CTC model so the aligner consumes one phone set.
class PhonemeLexicon {
 public:
  // Parses "word<TAB>phone phone ..." rows. Throws on unreadable input.
  [[nodiscard]] static PhonemeLexicon load(
      const std::filesystem::path& dictionary_path);

  // Resolves the bundle's pronunciation_dictionary file through
  // model.svpmodel.json and loads it. Throws when the bundle does not
  // declare one.
  [[nodiscard]] static PhonemeLexicon load_from_bundle(
      const std::filesystem::path& bundle_dir);

  // espeak phone sequence for a normalized word, or nullopt when the word
  // cannot be resolved by the dictionary or the suffix table.
  [[nodiscard]] std::optional<std::vector<std::string>> phones_for(
      const std::string& normalized_word) const;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  std::unordered_map<std::string, std::vector<std::string>> entries_;
};

}  // namespace svp::audio
