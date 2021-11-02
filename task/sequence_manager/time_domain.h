// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TASK_SEQUENCE_MANAGER_TIME_DOMAIN_H_
#define BASE_TASK_SEQUENCE_MANAGER_TIME_DOMAIN_H_

#include "base/callback.h"
#include "base/check.h"
#include "base/containers/intrusive_heap.h"
#include "base/task/sequence_manager/lazy_now.h"
#include "base/task/sequence_manager/task_queue_impl.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "base/values.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {
namespace sequence_manager {

class SequenceManager;

namespace internal {
class AssociatedThreadId;
class SequenceManagerImpl;
class TaskQueueImpl;
}  // namespace internal

// TimeDomain wakes up TaskQueues when their delayed tasks are due to run.
// This class allows overrides to enable clock overriding on some TaskQueues
// (e.g. auto-advancing virtual time, throttled clock, etc).
//
// TaskQueue maintains its own next wake-up time and communicates it
// to the TimeDomain, which aggregates wake-ups across registered TaskQueues
// into a global wake-up, which ultimately gets passed to the ThreadController.
class BASE_EXPORT TimeDomain : public TickClock {
 public:
  TimeDomain(const TimeDomain&) = delete;
  TimeDomain& operator=(const TimeDomain&) = delete;
  ~TimeDomain() override;

  // Returns the ready time for the next pending delayed task, is_null() if the
  // next task can run immediately, or is_max() if there are no more delayed
  // tasks. Can be called from main thread only. NOTE: |lazy_now| and the return
  // value are in the SequenceManager's time.
  virtual TimeTicks GetNextDelayedTaskTime(LazyNow* lazy_now) const = 0;

  Value AsValue() const;

  bool has_pending_high_resolution_tasks() const {
    return pending_high_res_wake_up_count_;
  }

  // Returns true if there are no pending delayed tasks.
  bool empty() const { return delayed_wake_up_queue_.empty(); }

  // This is the signal that virtual time should step forward. If
  // RunLoop::QuitWhenIdle has been called then |quit_when_idle_requested| will
  // be true. Returns true if there is a task to run now.
  virtual bool MaybeFastForwardToNextTask(bool quit_when_idle_requested) = 0;

 protected:
  TimeDomain();

  SequenceManager* sequence_manager() const;

  // Returns a DelayedWakeUp for the next pending delayed task (pending delayed
  // tasks that are ripe may be ignored if they have already been moved to a
  // ready queue). If there are no such tasks (immediate tasks don't count) or
  // queues are disabled it returns nullopt.
  absl::optional<DelayedWakeUp> GetNextDelayedWakeUp() const;

  size_t NumberOfScheduledWakeUps() const {
    return delayed_wake_up_queue_.size();
  }

  // Tells SequenceManager to schedule delayed work, use TimeTicks::Max()
  // to unschedule. Also cancels any previous requests.
  // May be overriden to control wake ups manually.
  virtual void SetNextDelayedDoWork(LazyNow* lazy_now, TimeTicks run_time);

  // Tells SequenceManager to schedule immediate work.
  // May be overriden to control wake ups manually.
  virtual void RequestDoWork();

  virtual const char* GetName() const = 0;

  // Called when the TimeDomain is registered. |sequence_manager| is expected to
  // be valid for the duration of TimeDomain's existence.
  // TODO(scheduler-dev): Pass SequenceManager in the constructor.
  virtual void OnRegisterWithSequenceManager(
      internal::SequenceManagerImpl* sequence_manager);

  // Removes all canceled delayed tasks from the front of the queue. After
  // calling this, GetNextDelayedWakeUp() is guaranteed to return a wake up time
  // for a non-canceled task.
  void RemoveAllCanceledDelayedTasksFromFront(LazyNow* lazy_now);

 private:
  friend class internal::TaskQueueImpl;
  friend class internal::SequenceManagerImpl;
  friend class TestTimeDomain;

  // Schedule TaskQueue to wake up at certain time, repeating calls with
  // the same |queue| invalidate previous requests.
  // Nullopt |wake_up| cancels a previously set wake up for |queue|.
  // NOTE: |lazy_now| is provided in TimeDomain's time.
  void SetNextWakeUpForQueue(internal::TaskQueueImpl* queue,
                             absl::optional<DelayedWakeUp> wake_up,
                             LazyNow* lazy_now);

  // Remove the TaskQueue from any internal data sctructures.
  void UnregisterQueue(internal::TaskQueueImpl* queue);

  // Wake up each TaskQueue where the delay has elapsed. Note this doesn't
  // ScheduleWork.
  void MoveReadyDelayedTasksToWorkQueues(LazyNow* lazy_now);

  struct ScheduledDelayedWakeUp {
    DelayedWakeUp wake_up;
    internal::TaskQueueImpl* queue;

    bool operator>(const ScheduledDelayedWakeUp& other) const {
      return wake_up > other.wake_up;
    }

    void SetHeapHandle(HeapHandle handle) {
      DCHECK(handle.IsValid());
      queue->set_heap_handle(handle);
    }

    void ClearHeapHandle() {
      DCHECK(queue->heap_handle().IsValid());
      queue->set_heap_handle(HeapHandle());
    }

    HeapHandle GetHeapHandle() const { return queue->heap_handle(); }
  };

  internal::SequenceManagerImpl* sequence_manager_ = nullptr;  // Not owned.
  IntrusiveHeap<ScheduledDelayedWakeUp, std::greater<>> delayed_wake_up_queue_;
  int pending_high_res_wake_up_count_ = 0;

  scoped_refptr<internal::AssociatedThreadId> associated_thread_;
};

}  // namespace sequence_manager
}  // namespace base

#endif  // BASE_TASK_SEQUENCE_MANAGER_TIME_DOMAIN_H_
