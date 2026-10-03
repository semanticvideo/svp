#include "svp/exec/worker/slot_sharing.hpp"

#include <limits>
#include <tuple>
#include <utility>

namespace svp::exec::worker {

SlotSharing::SlotSharing(std::chrono::milliseconds contention_window, LocalLoad local_load,
                         Clock clock)
    : contention_window_(contention_window),
      local_load_(std::move(local_load)),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })) {}

std::uint64_t SlotSharing::held_by(std::string_view coordinator,
                                   std::string_view task_type) const {
  std::uint64_t count = 0;
  for (const auto& [key, holder] : held_) {
    if (holder.coordinator == coordinator && holder.task_type == task_type) {
      ++count;
    }
  }
  return count;
}

std::uint64_t SlotSharing::held(std::string_view task_type) const {
  const std::lock_guard lock(mutex_);
  std::uint64_t count = 0;
  for (const auto& [key, holder] : held_) {
    if (holder.task_type == task_type) {
      ++count;
    }
  }
  return count;
}

void SlotSharing::expire_waiting(std::chrono::steady_clock::time_point now) {
  for (auto type = waiting_.begin(); type != waiting_.end();) {
    std::erase_if(type->second, [&](const auto& entry) {
      return now - entry.second.last_asked > contention_window_;
    });
    type = type->second.empty() ? waiting_.erase(type) : std::next(type);
  }
}

void SlotSharing::wait(const SlotRequest& request, std::chrono::steady_clock::time_point now) {
  auto& waiting = waiting_[request.task_type];
  const auto [entry, inserted] = waiting.try_emplace(request.coordinator);
  if (inserted) {
    entry->second.ticket = next_ticket_++;
  }
  entry->second.last_asked = now;
}

AdmissionDecision SlotSharing::try_take(const SlotRequest& request) {
  const TaskTypeCounts local = local_load_ ? local_load_() : TaskTypeCounts{};
  const std::lock_guard lock(mutex_);
  const auto now = clock_();
  expire_waiting(now);
  const auto record = [&] {
    held_[{request.session, request.lease_id}] =
        Held{.coordinator = request.coordinator, .task_type = request.task_type};
    if (const auto type = waiting_.find(request.task_type); type != waiting_.end()) {
      type->second.erase(request.coordinator);
      if (type->second.empty()) {
        waiting_.erase(type);
      }
    }
    return AdmissionDecision{.admitted = true, .code = {}, .message = {}};
  };
  if (request.declared_slots == 0) {
    return record();
  }

  const auto local_running = local.find(request.task_type);
  const std::uint64_t running_locally = local_running == local.end() ? 0 : local_running->second;
  std::uint64_t held_by_others = 0;
  std::uint64_t held_by_requester = 0;
  for (const auto& [key, holder] : held_) {
    if (holder.task_type != request.task_type) {
      continue;
    }
    if (holder.coordinator == request.coordinator) {
      ++held_by_requester;
    } else {
      ++held_by_others;
    }
  }
  const auto type_waiting = waiting_.find(request.task_type);
  bool others_waiting = false;
  if (type_waiting != waiting_.end()) {
    for (const auto& [coordinator, entry] : type_waiting->second) {
      others_waiting = others_waiting || coordinator != request.coordinator;
    }
  }
  if (held_by_others == 0 && running_locally == 0 && !others_waiting) {
    return record();
  }

  const std::uint64_t running = held_by_requester + held_by_others + running_locally;
  if (running >= request.declared_slots) {
    wait(request, now);
    return AdmissionDecision{
        .admitted = false,
        .code = std::string(kRejectInsufficientSlots),
        .message = "all " + std::to_string(request.declared_slots) + " " + request.task_type +
                   " slot(s) of this worker are in use (" + std::to_string(held_by_others) +
                   " by other coordinators, " + std::to_string(running_locally) +
                   " by this Mac's own build)"};
  }
  if (others_waiting) {
    std::uint64_t own_ticket = std::numeric_limits<std::uint64_t>::max();
    if (const auto own = type_waiting->second.find(request.coordinator);
        own != type_waiting->second.end()) {
      own_ticket = own->second.ticket;
    }
    const auto own_key = std::make_tuple(held_by_requester, own_ticket);
    for (const auto& [coordinator, entry] : type_waiting->second) {
      if (coordinator == request.coordinator) {
        continue;
      }
      if (std::make_tuple(held_by(coordinator, request.task_type), entry.ticket) < own_key) {
        wait(request, now);
        return AdmissionDecision{
            .admitted = false,
            .code = std::string(kRejectInsufficientSlots),
            .message = "the free " + request.task_type +
                       " slot of this worker is kept for a coordinator that has been waiting "
                       "for one"};
      }
    }
  }
  return record();
}

void SlotSharing::release(std::string_view session, std::string_view lease_id) {
  const std::lock_guard lock(mutex_);
  held_.erase({std::string(session), std::string(lease_id)});
}

void SlotSharing::release_session(std::string_view session) {
  const std::lock_guard lock(mutex_);
  std::erase_if(held_, [&](const auto& entry) { return entry.first.first == session; });
}

}  // namespace svp::exec::worker
