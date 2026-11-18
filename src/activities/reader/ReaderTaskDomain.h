#pragma once

#include <cstdint>

#include "ReaderJobQueue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// r163 C2 -- lazy-spawn, self-idling task domains with bounded queues.
//
// The vendor runs five fixed domains (main/application_tasks.cc:20-40, :82): a task is created on
// its first job at priority 3 and deleted again after 2 s idle; submission returns false instead of
// blocking when the queue is full; an accepted job takes ownership of its context; nothing on the
// hot path allocates.
//
// Of those five we gave exactly one a task (ProgressWriter.h says why). This file is the mechanism
// it needs and nothing more: a fixed-capacity job queue, and a domain that creates a task for its
// first job and deletes itself when it runs dry.
//
// A job is a value: it carries its own context and exposes run(). No function pointers, no
// captures, no allocation -- the queue holds the job by value, so "accepted" means "the domain now
// owns it" and the producer may forget it immediately.

namespace readertasks {

// One domain: its queue, the task that drains it, and the state that decides when the task goes
// away again. The task holds no pointer to the producer, so a domain outliving its caller is not a
// dangling read -- which is what lets a queued write finish while the reader is already gone.
template <typename Job, int Capacity>
class Domain {
 public:
  struct Config {
    const char* name = "reader-job";
    // Vendor reference: 3. See ProgressWriter.h for the numbers we chose and why.
    uint32_t stackBytes = 3072;
    UBaseType_t priority = 1;
    // A task that has seen no job for this long deletes itself; the next submit creates a new one.
    uint32_t idleExitMs = 2000;
  };

  // Created on first use, so a domain costs its semaphores and nothing else until it is needed.
  bool begin(const Config& config) {
    config_ = config;
    if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
    if (wake_ == nullptr) wake_ = xSemaphoreCreateBinary();
    return mutex_ != nullptr && wake_ != nullptr;
  }

  // False when the job was NOT accepted: the queue is full, or the task could not be created. The
  // caller keeps ownership and must do something else with it -- retry, keep it pending, or report.
  bool submit(const Job& job) {
    if (mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    switch (submitAction(handle_ != nullptr, queue_.count(), Capacity)) {
      case SubmitAction::RefuseQueueFull:
        xSemaphoreGive(mutex_);
        return false;  // bounded: the caller keeps the job
      case SubmitAction::SpawnThenAccept:
        if (!spawn()) {
          xSemaphoreGive(mutex_);
          return false;
        }
        [[fallthrough]];
      case SubmitAction::Accept:
        break;
    }
    queue_.push(job);  // the action above proved there is room
    xSemaphoreGive(mutex_);
    xSemaphoreGive(wake_);
    return true;
  }

  int queued() {
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const int count = queue_.count();
    xSemaphoreGive(mutex_);
    return count;
  }

  // Tasks created so far: one per idle-exit cycle, i.e. the proof that a quiet reader costs no task.
  int spawnedTasks() {
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const int count = spawned_;
    xSemaphoreGive(mutex_);
    return count;
  }

 private:
  // Called with the mutex held.
  bool spawn() {
    const BaseType_t created =
        xTaskCreate(&Domain::trampoline, config_.name, config_.stackBytes, this, config_.priority, &handle_);
    if (created != pdPASS) {
      handle_ = nullptr;
      return false;
    }
    spawned_++;
    return true;
  }

  static void trampoline(void* self) { static_cast<Domain*>(self)->run(); }

  void run() {
    Job job;
    for (;;) {
      if (take(job)) {
        job.run();
        continue;
      }
      // Nothing queued: sleep until a job arrives, or until this task has been idle long enough.
      if (xSemaphoreTake(wake_, pdMS_TO_TICKS(config_.idleExitMs)) == pdTRUE) continue;

      // Idle timeout. Decide under the mutex, so a submit either sees this task leaving (handle_ is
      // null) and spawns a replacement, or its job is found here and this task carries on. The
      // queue is read directly: take() would lock the mutex a second time.
      xSemaphoreTake(mutex_, portMAX_DELAY);
      if (!exitWhenIdle(queue_.count())) {
        queue_.pop(job);
        xSemaphoreGive(mutex_);
        job.run();
        continue;
      }
      handle_ = nullptr;
      xSemaphoreGive(mutex_);
      // Not holding the mutex: deleting a task that owns a mutex would strand it. A submit that
      // arrives from here on spawns a new task; this one touches nothing again.
      vTaskDelete(nullptr);
    }
  }

  bool take(Job& job) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool got = queue_.pop(job);
    xSemaphoreGive(mutex_);
    return got;
  }

  Config config_{};
  JobQueue<Job, Capacity> queue_{};
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t wake_ = nullptr;
  TaskHandle_t handle_ = nullptr;
  int spawned_ = 0;
};

}  // namespace readertasks
