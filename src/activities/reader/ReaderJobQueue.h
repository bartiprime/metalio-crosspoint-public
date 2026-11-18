#pragma once

#include <cstdint>
#include <type_traits>

// r163 C2 -- the host-testable half of a lazy-spawn, self-idling task domain.
//
// ReaderTaskDomain.h adds the FreeRTOS task to this; everything that can be wrong in a way the host
// can see lives here: the queue's bounds, the submit decision (accept / spawn / refuse) and the
// idle-exit decision. Those three are the whole contract the reader relies on --
//
//   * bounded: a full queue answers false, it never blocks and never grows;
//   * accepted means owned: the queue holds the job by value, so the producer forgets it;
//   * idle means gone: a task with nothing queued exits and the next submit creates one.

namespace readertasks {

// Bounded, fixed-capacity, no allocation. Not thread-safe by itself: a Domain serialises access
// with its own mutex (its two producers are the render task and the loop task).
template <typename Job, int Capacity>
class JobQueue {
  static_assert(Capacity > 0, "a domain needs room for at least one job");
  static_assert(std::is_trivially_copyable_v<Job>, "a job owns its context by value: no pointers into caller state");

 public:
  bool push(const Job& job) {
    if (count_ == Capacity) return false;
    slots_[(head_ + count_) % Capacity] = job;
    ++count_;
    return true;
  }

  bool pop(Job& out) {
    if (count_ == 0) return false;
    out = slots_[head_];
    head_ = (head_ + 1) % Capacity;
    --count_;
    return true;
  }

  int count() const { return count_; }
  static constexpr int capacity() { return Capacity; }

 private:
  Job slots_[Capacity]{};
  int head_ = 0;
  int count_ = 0;
};

// What a submit must do. Spawn is separate from accept because creating the task can fail, and a
// refused job must stay with the caller rather than disappear.
enum class SubmitAction {
  Accept,          // a task is alive and the queue has room
  SpawnThenAccept, // the queue has room but no task is alive (it exited when it ran dry)
  RefuseQueueFull, // bounded: the caller keeps the job and does something else with it
};

inline SubmitAction submitAction(const bool taskAlive, const int queued, const int capacity) {
  if (queued >= capacity) return SubmitAction::RefuseQueueFull;
  return taskAlive ? SubmitAction::Accept : SubmitAction::SpawnThenAccept;
}

// A task that has been idle for its timeout exits only when it has nothing left to do. Checked
// under the domain's mutex, so a submit either lands in the queue before this decision or sees the
// task gone and creates a new one.
inline bool exitWhenIdle(const int queued) { return queued == 0; }

}  // namespace readertasks
