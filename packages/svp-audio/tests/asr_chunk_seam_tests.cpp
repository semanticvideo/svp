#include "audio_test_support.hpp"

// Seams between two overlapping ASR chunks. Each case is a pattern from a
// real long-form transcript: chunk 0 ends while a word is still being spoken,
// chunk 1 starts 2 s earlier, and reconcile_overlapping_chunks must keep one
// copy of the overlap's speech. Times are source milliseconds.

namespace {

using svp::audio::AsrWord;

constexpr std::int64_t kUsPerMs = 1000;

// A 30 s plan of 15 s chunks overlapping by 2 s: chunk 1 covers 13..28 s and
// its first 2 s are chunk 0's last 2 s.
svp::audio::AsrChunkPlanResult seam_plan() {
  const auto plan =
      svp::audio::build_asr_chunk_plan(30000000, 15000000, 2000000);
  assert(plan.chunks.size() == 3);
  assert(plan.chunks[1].source_start_us == 13000000);
  assert(plan.chunks[1].overlap_before_us == 2000000);
  return plan;
}

// A word chunk 0 heard.
AsrWord prior(const std::string& text, std::int64_t start_ms,
              std::int64_t end_ms) {
  return {text, start_ms * kUsPerMs, end_ms * kUsPerMs, 0.9, 0};
}

// A word chunk 1 heard, stored relative to its chunk's start.
AsrWord incoming(const std::string& text, std::int64_t start_ms,
                 std::int64_t end_ms) {
  const std::int64_t chunk_start_ms = 13000;
  return {text, (start_ms - chunk_start_ms) * kUsPerMs,
          (end_ms - chunk_start_ms) * kUsPerMs, 0.9, 1};
}

std::vector<AsrWord> reconcile_seam(std::vector<AsrWord> prior_words,
                                    std::vector<AsrWord> incoming_words) {
  const auto plan = seam_plan();
  std::vector<std::vector<AsrWord>> chunk_words(plan.chunks.size());
  chunk_words[0] = std::move(prior_words);
  chunk_words[1] = std::move(incoming_words);
  const auto reconciled =
      svp::audio::reconcile_overlapping_chunks(chunk_words, plan.chunks);
  for (std::size_t i = 0; i < reconciled.size(); ++i) {
    assert(reconciled[i].end_us > reconciled[i].start_us);
    if (i > 0) assert(reconciled[i].start_us >= reconciled[i - 1].end_us);
  }
  return reconciled;
}

std::string joined_text(const std::vector<AsrWord>& words) {
  std::string text;
  for (const AsrWord& word : words) {
    if (!text.empty()) text += ' ';
    text += word.text;
  }
  return text;
}

const AsrWord& word_named(const std::vector<AsrWord>& words,
                          const std::string& text) {
  const auto found =
      std::find_if(words.begin(), words.end(),
                   [&](const AsrWord& word) { return word.text == text; });
  assert(found != words.end());
  return *found;
}

}  // namespace

// "tons of leaves. lead on that one": chunk 0 heard the start of "lead" as
// "leaves." at its very end; most of that word lies past the seam.
void test_overlap_reconciliation_seam_drops_cut_off_prior_word_past_the_seam() {
  const auto reconciled = reconcile_seam(
      {prior("quite", 12748, 13200), prior("often", 13220, 13560),
       prior("now.", 13570, 13660), prior("She", 13744, 14000),
       prior("still", 14028, 14280), prior("brings", 14308, 14560),
       prior("tons", 14592, 14840), prior("of", 14850, 14900),
       prior("leaves.", 14900, 15000)},
      {incoming("often", 13220, 13560), incoming("now.", 13570, 13660),
       incoming("She", 13744, 14000), incoming("still", 14028, 14280),
       incoming("brings", 14308, 14560), incoming("tons", 14592, 14840),
       incoming("of", 14850, 14940), incoming("lead", 14942, 15300),
       incoming("on", 15397, 16380), incoming("that", 16400, 16560),
       incoming("one.", 16570, 16780)});
  assert(joined_text(reconciled) ==
         "quite often now. She still brings tons of lead on that one.");
  assert(word_named(reconciled, "of").end_us == 14900 * kUsPerMs);
  assert(word_named(reconciled, "lead").start_us == 14942 * kUsPerMs);
}

// "the Four-Way Partnership. four way partnership": chunk 0's words after
// the anchor start at or after the seam, or reach well past it. They used to
// stay in the transcript at zero or near-zero length.
void test_overlap_reconciliation_seam_drops_prior_words_that_collapsed_to_the_seam() {
  const auto reconciled = reconcile_seam(
      {prior("Diane", 13116, 13540), prior("Barnes", 13560, 13920),
       prior("and", 13952, 14100), prior("the", 14100, 14120),
       prior("Four-Way", 14120, 14400), prior("Partnership.", 14400, 15000)},
      {incoming("Diane", 13116, 13540), incoming("Barnes", 13560, 13920),
       incoming("and", 13952, 14100), incoming("the", 14100, 14170),
       incoming("four", 14175, 14380), incoming("way", 14390, 14440),
       incoming("partnership", 14479, 15080),
       incoming("signed", 15188, 15740)});
  assert(joined_text(reconciled) ==
         "Diane Barnes and the four way partnership signed");
  assert(word_named(reconciled, "the").end_us == 14120 * kUsPerMs);
}

// "going to turn into like... turn into like commercial": both chunks heard
// "turn into like" in the anchor, but chunk 0 times its copy at the seam,
// where chunk 1's continuation has its own.
void test_overlap_reconciliation_seam_drops_prior_anchor_copy_timed_past_the_seam() {
  const auto reconciled = reconcile_seam(
      {prior("it", 13800, 13940), prior("was", 13940, 14060),
       prior("going", 14080, 14220), prior("to", 14230, 14250),
       prior("turn", 14260, 14480), prior("into", 14500, 14700),
       prior("like...", 14720, 15000)},
      {incoming("it", 13800, 13900), incoming("was", 13900, 13980),
       incoming("going", 13980, 14050), incoming("to", 14050, 14080),
       incoming("turn", 14080, 14140), incoming("into", 14140, 14200),
       incoming("like", 14200, 14250), incoming("turn", 14258, 14520),
       incoming("into", 14530, 14700), incoming("like", 14728, 15060),
       incoming("commercial", 15238, 16000)});
  assert(joined_text(reconciled) ==
         "it was going to turn into like commercial");
  assert(word_named(reconciled, "turn").chunk_ordinal == 1);
  assert(word_named(reconciled, "turn").start_us == 14258 * kUsPerMs);
}

// "So proud of all. all of you guys.": chunk 0's cut-off "all." ends before
// chunk 1's "all" starts, so timestamps alone cannot tell it is a copy; the
// anchor does.
void test_overlap_reconciliation_seam_drops_cut_off_prior_word_before_the_seam() {
  const auto reconciled = reconcile_seam(
      {prior("continue", 13070, 13420), prior("to", 13441, 13500),
       prior("do", 13500, 13580), prior("more", 13582, 13720),
       prior("deals", 13773, 14040), prior("together.", 14060, 14340),
       prior("So", 14369, 14540), prior("proud", 14582, 14880),
       prior("of", 14890, 14940), prior("all.", 14940, 14980)},
      {incoming("continue", 13070, 13420), incoming("to", 13441, 13500),
       incoming("do", 13500, 13580), incoming("more", 13582, 13720),
       incoming("deals", 13773, 14040), incoming("together.", 14060, 14340),
       incoming("So", 14369, 14540), incoming("proud", 14582, 14880),
       incoming("of", 14890, 14990), incoming("all", 15008, 15220),
       incoming("of", 15220, 15320), incoming("you", 15320, 15360),
       incoming("guys.", 15381, 15680)});
  assert(joined_text(reconciled) ==
         "continue to do more deals together. So proud of all of you guys.");
}

// "we bought the park right going into busy season": the anchor's last word
// is only in chunk 0's copy of the transcript, so it stays even when most of
// it reaches past the seam; it ends at the seam.
void test_overlap_reconciliation_seam_trims_anchor_word_reaching_past_the_seam() {
  const auto reconciled = reconcile_seam(
      {prior("But", 12542, 12800), prior("we", 12939, 13260),
       prior("bought", 13291, 13540), prior("the", 13550, 13620),
       prior("park", 13651, 14600)},
      {incoming("we", 13000, 13260), incoming("bought", 13291, 13540),
       incoming("the", 13550, 13620), incoming("park", 13651, 14000),
       incoming("right", 14046, 14180), incoming("going", 14240, 14560),
       incoming("into", 14570, 14740), incoming("busy", 14763, 14980),
       incoming("season", 15048, 15580)});
  assert(joined_text(reconciled) ==
         "But we bought the park right going into busy season");
  const AsrWord& park = word_named(reconciled, "park");
  assert(park.chunk_ordinal == 0);
  assert(park.start_us == 13651 * kUsPerMs);
  assert(park.end_us == 14046 * kUsPerMs);
}

// "for multiple, multiple months": a repetition with its own audio on each
// side of an anchored seam is speech, not a seam copy.
void test_overlap_reconciliation_seam_keeps_repetition_across_anchored_seam() {
  const auto reconciled = reconcile_seam(
      {prior("and", 12650, 12830), prior("the", 12840, 12880),
       prior("team", 12905, 13180), prior("follows", 13235, 13620),
       prior("up", 13632, 13800), prior("with", 13800, 13900),
       prior("the", 13910, 13980), prior("sellers", 14008, 14460),
       prior("for", 14540, 14740), prior("multiple,", 14835, 15000)},
      {incoming("follows", 13235, 13620), incoming("up", 13632, 13800),
       incoming("with", 13800, 13900), incoming("the", 13910, 13980),
       incoming("sellers", 14008, 14460), incoming("for", 14540, 14740),
       incoming("multiple,", 14835, 15440),
       incoming("multiple", 15490, 15880), incoming("months.", 15880, 15980),
       incoming("In", 16217, 16380), incoming("fact,", 16412, 16740)});
  assert(joined_text(reconciled) ==
         "and the team follows up with the sellers for multiple, multiple "
         "months. In fact,");
}

// "for you, you gotta": with no anchor, a repetition that starts after
// chunk 0's last word ends is kept.
void test_overlap_reconciliation_seam_keeps_repetition_across_unanchored_seam() {
  const auto reconciled = reconcile_seam(
      {prior("the", 12580, 12620), prior("work", 12661, 13060),
       prior("for", 13206, 13520), prior("you,", 13524, 13720)},
      {incoming("for", 13206, 13520), incoming("you,", 13524, 13720),
       incoming("you", 13944, 14040), incoming("gotta", 14088, 14360),
       incoming("take", 14370, 14560), incoming("care", 14582, 14680),
       incoming("of", 14708, 14880), incoming("them.", 14880, 14980),
       incoming("And", 15274, 15700)});
  assert(joined_text(reconciled) ==
         "the work for you, you gotta take care of them. And");
  assert(word_named(reconciled, "you,").chunk_ordinal == 0);
  assert(word_named(reconciled, "you").chunk_ordinal == 1);
}

// A phrase chunk 0 also said seconds earlier matches the overlap as well as
// its seam copy does. That earlier match is not the seam, so the words chunk
// 0 heard after it all stay.
void test_overlap_reconciliation_seam_ignores_anchor_repeated_earlier_in_prior() {
  const auto reconciled = reconcile_seam(
      {prior("I", 3000, 3100), prior("don't", 3100, 3300),
       prior("know", 3300, 3500), prior("if", 3500, 3700),
       prior("we", 3800, 4000), prior("went", 4000, 4200),
       prior("there", 4200, 4500), prior("and", 13900, 14100),
       prior("I", 14200, 14300), prior("don't", 14300, 14500),
       prior("know", 14500, 14700), prior("if", 14700, 14950)},
      {incoming("I", 14200, 14300), incoming("don't", 14300, 14500),
       incoming("know", 14500, 14700), incoming("if", 14700, 14950),
       incoming("we", 15050, 15300), incoming("should", 15300, 15600)});
  assert(joined_text(reconciled) ==
         "I don't know if we went there and I don't know if we should");
}
