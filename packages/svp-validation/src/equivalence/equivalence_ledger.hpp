#pragma once

#include "svp/validation/package_equivalence.hpp"

#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace svp::validation::equivalence {

// Whether a larger measured value is better (IoU, cosine, correlation) or
// worse (absolute error). Used to keep the worst observation when several
// occurrences of one rule are folded into a single finding.
enum class MetricDirection {
  lower_is_better,
  higher_is_better,
};

// A digest field whose mismatch is decided by the equivalence of the source
// layers it hashes (RC2 Sections 5.16.2 and 17.5 hash rule).
struct DeferredDigestCheck {
  std::string entry;
  std::string location;
  std::string rule;
  std::vector<std::string> source_keys;
};

// Collects findings, folds repeated occurrences of one rule inside one entry
// into a single finding that keeps the first location, and tracks the worst
// outcome per comparison source key (an entry path or a block key).
class EquivalenceLedger {
 public:
  void add(EquivalenceFinding finding,
           MetricDirection direction = MetricDirection::lower_is_better);

  // Records that a source key was compared, so digest checks can tell a
  // compared-and-equal layer from one that was never compared.
  void mark_compared(const std::string& source_key);
  void mark_outcome(const std::string& source_key, EquivalenceOutcome outcome);

  void defer_digest(DeferredDigestCheck check);
  void resolve_deferred_digests();

  [[nodiscard]] EquivalenceClass classification() const;
  [[nodiscard]] std::vector<EquivalenceFinding> findings() const;

 private:
  using FoldKey = std::tuple<std::string, std::string, EquivalenceOutcome>;

  std::map<FoldKey, std::size_t> fold_index_;
  std::vector<EquivalenceFinding> findings_;
  std::map<std::string, bool> source_not_equivalent_;
  std::vector<DeferredDigestCheck> deferred_;
};

[[nodiscard]] std::string block_source_key(const std::string& block_file,
                                           std::uint64_t block_offset);

}  // namespace svp::validation::equivalence
