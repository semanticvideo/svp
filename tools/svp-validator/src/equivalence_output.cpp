#include "equivalence_output.hpp"

#include <sstream>

namespace svp::validator_cli {
namespace {

void print_identity(std::ostream& output,
                    std::string_view label,
                    const svp::validation::EquivalencePackageIdentity& package) {
  output << label << ": " << package.path;
  if (!package.blake3.empty()) {
    output << " (" << package.size_bytes << " bytes, " << package.blake3 << ")";
  }
  output << "\n";
}

void print_finding(std::ostream& output, const svp::validation::EquivalenceFinding& finding) {
  output << "  " << svp::validation::to_string(finding.outcome) << " [" << finding.rule
         << "] " << (finding.entry.empty() ? "-" : finding.entry);
  if (!finding.location.empty()) {
    output << " " << finding.location;
  }
  if (finding.measured.has_value()) {
    output << " measured=" << *finding.measured;
  }
  if (finding.tolerance.has_value()) {
    output << " tolerance=" << *finding.tolerance;
  }
  if (finding.occurrences > 1) {
    output << " occurrences=" << finding.occurrences;
  }
  if (!finding.detail.empty()) {
    output << ": " << finding.detail;
  }
  output << "\n";
}

}  // namespace

void print_equivalence_report(std::ostream& output,
                              const svp::validation::EquivalenceReport& report) {
  output << "SVP equivalence: " << svp::validation::to_string(report.classification) << "\n";
  output << "Profile: " << report.profile << "\n";
  output << "Build metadata normalization: "
         << (report.normalize_build_metadata ? "on" : "off") << "\n";
  print_identity(output, "Left", report.left);
  print_identity(output, "Right", report.right);
  output << "Entries compared: " << report.entries_compared << "\n";

  if (report.findings.empty()) {
    output << "No findings.\n";
  } else {
    output << "Findings:\n";
    for (const auto& finding : report.findings) {
      print_finding(output, finding);
    }
  }

  if (report.classification != svp::validation::EquivalenceClass::byte_identical) {
    output << "Compared exactly (no Default Equivalence Profile v1 rule evaluated):\n";
    for (const auto& note : report.exact_fallbacks) {
      output << "  - " << note << "\n";
    }
  }
}

}  // namespace svp::validator_cli
