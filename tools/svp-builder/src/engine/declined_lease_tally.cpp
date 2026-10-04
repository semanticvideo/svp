#include "engine/declined_lease_tally.hpp"

#include <sstream>

namespace svp::builder::engine {
namespace {

// The scheduler reports a rejection's detail as "<code>: <message>"
// (scheduler_run.cpp on_rejected).
constexpr std::string_view kCodeSeparator = ": ";

std::string code_of(const std::string& detail) {
  const std::size_t end = detail.find(kCodeSeparator);
  return end == std::string::npos ? detail : detail.substr(0, end);
}

}  // namespace

void DeclinedLeaseTally::observe(const svp::exec::AttemptEvent& event) {
  if (event.kind != svp::exec::AttemptEventKind::rejected) {
    return;
  }
  Executor& executor = executors_[event.executor_id];
  ++executor.total;
  ++executor.by_code[code_of(event.detail)];
  executor.last_detail = event.detail;
}

std::uint64_t DeclinedLeaseTally::declined(std::string_view executor_id) const {
  const auto found = executors_.find(executor_id);
  return found == executors_.end() ? 0 : found->second.total;
}

std::string DeclinedLeaseTally::summary(std::string_view stage) const {
  std::ostringstream out;
  for (const auto& [id, executor] : executors_) {
    out << "svp-builder: " << stage << ": " << id << " declined " << executor.total
        << " lease(s):";
    const char* separator = " ";
    for (const auto& [code, count] : executor.by_code) {
      out << separator << (code.empty() ? std::string("unspecified") : code) << " " << count;
      separator = ", ";
    }
    out << "; last: " << executor.last_detail << "\n";
  }
  return out.str();
}

}  // namespace svp::builder::engine
