#include "svp/exec/worker/memory_turns.hpp"

#include <algorithm>
#include <iterator>

namespace svp::exec::worker {

MemoryTurns::MemoryTurns(MemoryTurnPolicy policy) : policy_(policy) {}

void MemoryTurns::end_fruitless(std::map<std::string, Wait, std::less<>>::iterator wait,
                                TimePoint now) {
  // Rest as long as the wait held the other types back.
  rests_.insert_or_assign(wait->first, now + (now - wait->second.since));
  waits_.erase(wait);
}

void MemoryTurns::expire(TimePoint now) {
  for (auto wait = waits_.begin(); wait != waits_.end();) {
    if (now - wait->second.last_asked > policy_.idle_window) {
      wait = waits_.erase(wait);
    } else if (now - wait->second.since >= policy_.max_wait) {
      const auto next = std::next(wait);
      end_fruitless(wait, now);
      wait = next;
    } else {
      ++wait;
    }
  }
  std::erase_if(rests_, [&](const auto& rest) { return rest.second <= now; });
}

std::optional<std::string> MemoryTurns::yields_to(std::string_view task_type) const {
  const auto own = waits_.find(task_type);
  const std::uint64_t own_ticket = own == waits_.end() ? next_ticket_ : own->second.ticket;
  const std::string* first = nullptr;
  std::uint64_t first_ticket = own_ticket;
  for (const auto& [type, wait] : waits_) {
    if (type != task_type && wait.ticket < first_ticket) {
      first = &type;
      first_ticket = wait.ticket;
    }
  }
  return first == nullptr ? std::nullopt : std::optional<std::string>(*first);
}

void MemoryTurns::refused(std::string_view task_type,
                          const std::set<LeaseKey>& other_types_running, TimePoint now) {
  if (const auto wait = waits_.find(task_type); wait != waits_.end()) {
    wait->second.last_asked = now;
    const bool any_left = std::any_of(
        wait->second.waiting_for.begin(), wait->second.waiting_for.end(),
        [&](const LeaseKey& lease) { return other_types_running.contains(lease); });
    if (!any_left) {
      end_fruitless(wait, now);
    }
    return;
  }
  // Nothing of another type runs here: holding other types back cannot free
  // anything for this one.
  if (other_types_running.empty() || resting(task_type, now)) {
    return;
  }
  waits_.emplace(std::string(task_type), Wait{.ticket = next_ticket_++,
                                              .since = now,
                                              .last_asked = now,
                                              .waiting_for = other_types_running});
}

void MemoryTurns::admitted(std::string_view task_type) {
  if (const auto wait = waits_.find(task_type); wait != waits_.end()) {
    waits_.erase(wait);
  }
}

bool MemoryTurns::waiting(std::string_view task_type) const {
  return waits_.find(task_type) != waits_.end();
}

bool MemoryTurns::resting(std::string_view task_type, TimePoint now) const {
  const auto rest = rests_.find(task_type);
  return rest != rests_.end() && rest->second > now;
}

}  // namespace svp::exec::worker
