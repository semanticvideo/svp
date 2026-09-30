#pragma once

#include "svp/validation/package_equivalence.hpp"

#include <ostream>

namespace svp::validator_cli {

// Human-readable `validate --equivalent` report.
void print_equivalence_report(std::ostream& output,
                              const svp::validation::EquivalenceReport& report);

}  // namespace svp::validator_cli
