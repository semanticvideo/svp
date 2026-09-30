#include "svp/exec/cancellation_token.hpp"

#include "svp/exec/exec_error.hpp"

#include <string>

namespace svp::exec {

void throw_if_cancelled(const CancellationToken& cancellation, std::string_view where) {
  if (cancellation.requested()) {
    throw ExecError(ExecErrorCode::cancelled,
                    "attempt cancelled at " + std::string(where));
  }
}

}  // namespace svp::exec
