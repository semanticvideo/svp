#include "engine/whole_stage_scheduler_policy.hpp"

namespace svp::builder::engine {

svp::exec::SchedulerPolicy whole_stage_scheduler_policy() {
  svp::exec::SchedulerPolicy policy;
  policy.lease.lease_floor = kWholeStageAttemptBound;
  policy.lease.attempt_deadline_floor = kWholeStageAttemptBound;
  policy.retry.max_attempts = 1;
  svp::exec::validate_scheduler_policy(policy);
  return policy;
}

}  // namespace svp::builder::engine
