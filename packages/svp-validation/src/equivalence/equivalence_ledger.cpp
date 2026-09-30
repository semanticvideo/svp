#include "equivalence/equivalence_ledger.hpp"

#include <algorithm>
#include <utility>

namespace svp::validation::equivalence {
namespace {

bool is_worse(double candidate, double current, MetricDirection direction) {
  return direction == MetricDirection::lower_is_better ? candidate > current
                                                       : candidate < current;
}

}  // namespace

void EquivalenceLedger::add(EquivalenceFinding finding, MetricDirection direction) {
  mark_outcome(finding.entry, finding.outcome);

  FoldKey key{finding.entry, finding.rule, finding.outcome};
  const auto existing = fold_index_.find(key);
  if (existing == fold_index_.end()) {
    fold_index_.emplace(std::move(key), findings_.size());
    findings_.push_back(std::move(finding));
    return;
  }

  auto& folded = findings_[existing->second];
  folded.occurrences += finding.occurrences;
  if (finding.measured.has_value() &&
      (!folded.measured.has_value() ||
       is_worse(*finding.measured, *folded.measured, direction))) {
    folded.measured = finding.measured;
  }
}

void EquivalenceLedger::mark_compared(const std::string& source_key) {
  source_not_equivalent_.try_emplace(source_key, false);
}

void EquivalenceLedger::mark_outcome(const std::string& source_key,
                                     EquivalenceOutcome outcome) {
  auto& not_equivalent = source_not_equivalent_[source_key];
  not_equivalent = not_equivalent || outcome == EquivalenceOutcome::not_equivalent;
}

void EquivalenceLedger::defer_digest(DeferredDigestCheck check) {
  deferred_.push_back(std::move(check));
}

void EquivalenceLedger::resolve_deferred_digests() {
  auto pending = std::move(deferred_);
  deferred_.clear();
  for (auto& check : pending) {
    std::string failing_source;
    for (const auto& source : check.source_keys) {
      const auto status = source_not_equivalent_.find(source);
      if (status == source_not_equivalent_.end() || status->second) {
        failing_source = source;
        break;
      }
    }

    EquivalenceFinding finding{
        .entry = check.entry,
        .layer = "derived_digest",
        .rule = check.rule,
        .location = check.location,
    };
    if (failing_source.empty()) {
      finding.outcome =
          EquivalenceOutcome::hash_mismatch_allowed_by_source_layer_equivalence;
      finding.detail = "Digest differs; every hashed source layer compared as equivalent.";
    } else {
      finding.outcome = EquivalenceOutcome::not_equivalent;
      const bool compared = source_not_equivalent_.contains(failing_source);
      finding.detail = compared
          ? "Digest differs and hashed source layer '" + failing_source +
                "' is not equivalent."
          : "Digest differs and hashed source layer '" + failing_source +
                "' could not be compared.";
    }
    add(std::move(finding));
  }
}

EquivalenceClass EquivalenceLedger::classification() const {
  bool within_tolerance = false;
  for (const auto& finding : findings_) {
    if (finding.outcome == EquivalenceOutcome::not_equivalent) {
      return EquivalenceClass::not_equivalent;
    }
    within_tolerance =
        within_tolerance || finding.outcome == EquivalenceOutcome::within_tolerance;
  }
  return within_tolerance ? EquivalenceClass::numerically_equivalent
                          : EquivalenceClass::structurally_equivalent;
}

std::vector<EquivalenceFinding> EquivalenceLedger::findings() const {
  auto sorted = findings_;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const EquivalenceFinding& left, const EquivalenceFinding& right) {
                     return static_cast<int>(left.outcome) <
                            static_cast<int>(right.outcome);
                   });
  return sorted;
}

std::string block_source_key(const std::string& block_file, std::uint64_t block_offset) {
  return "block:" + block_file + "@" + std::to_string(block_offset);
}

}  // namespace svp::validation::equivalence
