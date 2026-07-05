#include "internal.hpp"

#include <iomanip>
#include <sstream>

namespace svp::audio::transcript_writer_internal {
namespace {

std::string word_id_for_ordinal(std::size_t ordinal) {
  std::ostringstream output;
  output << "word_" << std::setw(6) << std::setfill('0') << ordinal;
  return output.str();
}

}  // namespace

WordRecordBuildResult build_word_records(
    const std::vector<AsrWord>& words,
    const std::vector<std::string>& speaker_assignments) {
  WordRecordBuildResult result;
  for (std::size_t i = 0; i < words.size(); ++i) {
    const AsrWord& word = words[i];
    const std::string& speaker_id = speaker_assignments[i];
    result.speaker_intervals[speaker_id].push_back({word.start_us, word.end_us});
    result.records.push_back({
        {"id", word_id_for_ordinal(i)},
        {"text", word.text},
        {"start_us", word.start_us},
        {"end_us", word.end_us},
        {"confidence", word.confidence},
        {"chunk_ordinal", word.chunk_ordinal},
        {"speaker_id", speaker_id},
    });
  }
  return result;
}

}  // namespace svp::audio::transcript_writer_internal
