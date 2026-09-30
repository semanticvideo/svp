#pragma once

#include "svp/exec/result_commit_sink.hpp"
#include "svp/exec/task_graph.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace svp::exec {

// Reducer input for one lane (plan §4.5): the committed results of every task
// whose order key is in `lane`, sorted by order key, whatever order they
// completed or were passed in. Arrival order is never observable.
//
// Strict so a reducer never runs over a gap or an ambiguity: throws
// ExecError(invalid_value) when a result names a task the graph does not
// contain, when a task has more than one result, or when a task of the lane
// has none (plan §7.2: every planned unit commits or the build fails).
// Results of other lanes are ignored. The returned pointers alias `results`.
[[nodiscard]] std::vector<const CommittedResult*> results_in_canonical_order(
    const TaskGraph& graph, std::string_view lane,
    std::span<const CommittedResult> results);

}  // namespace svp::exec
