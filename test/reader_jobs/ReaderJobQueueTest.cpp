#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "activities/reader/ReaderJobQueue.h"

using readertasks::exitWhenIdle;
using readertasks::JobQueue;
using readertasks::submitAction;
using readertasks::SubmitAction;

namespace {

struct Job {
  int id = 0;
  int payload = 0;
};

// The firmware's domain loop, minus FreeRTOS: a task is alive or not, the queue is bounded, and a
// job is either accepted (and then run) or refused (and then still owned by the caller).
// This is the same pair of decisions Domain::submit() and Domain::run() make.
template <int Capacity>
class FakeDomain {
 public:
  bool submit(const Job& job) {
    switch (submitAction(taskAlive_, queue_.count(), Capacity)) {
      case SubmitAction::RefuseQueueFull:
        refusals_++;
        return false;
      case SubmitAction::SpawnThenAccept:
        taskAlive_ = true;  // xTaskCreate succeeded
        spawned_++;
        break;
      case SubmitAction::Accept:
        break;
    }
    queue_.push(job);
    return true;
  }

  // The task: drain everything, then exit if it has been idle. Returns the jobs it ran.
  std::vector<int> drain() {
    std::vector<int> ran;
    Job job;
    while (queue_.pop(job)) ran.push_back(job.id);
    if (taskAlive_ && exitWhenIdle(queue_.count())) taskAlive_ = false;
    return ran;
  }

  int queued() const { return queue_.count(); }
  int refusals() const { return refusals_; }
  int spawned() const { return spawned_; }
  bool taskAlive() const { return taskAlive_; }

 private:
  JobQueue<Job, Capacity> queue_;  // the real type: the same bounds the firmware enforces
  int refusals_ = 0;
  int spawned_ = 0;
  bool taskAlive_ = false;
};

}  // namespace

// ---- the queue ----------------------------------------------------------------------------

TEST(JobQueue, AcceptsUpToCapacityThenRefuses) {
  JobQueue<Job, 3> queue;
  EXPECT_TRUE(queue.push(Job{1, 0}));
  EXPECT_TRUE(queue.push(Job{2, 0}));
  EXPECT_TRUE(queue.push(Job{3, 0}));
  EXPECT_EQ(queue.count(), 3);
  EXPECT_FALSE(queue.push(Job{4, 0}));  // bounded: refused, not dropped, not grown
  EXPECT_EQ(queue.count(), 3);
}

TEST(JobQueue, PopsInOrderAndEmpties) {
  JobQueue<Job, 3> queue;
  for (int i = 0; i < 3; ++i) queue.push(Job{i, 0});
  Job out;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.id, i);
  }
  EXPECT_FALSE(queue.pop(out));
  EXPECT_EQ(queue.count(), 0);
}

TEST(JobQueue, RefusalClearsOnceJobsAreTaken) {
  JobQueue<Job, 2> queue;
  queue.push(Job{1, 0});
  queue.push(Job{2, 0});
  EXPECT_FALSE(queue.push(Job{3, 0}));
  Job out;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_TRUE(queue.push(Job{3, 0}));
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out.id, 2);
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out.id, 3);
}

// Accepted means owned: the queue holds a copy, so the producer may reuse its own object at once.
TEST(JobQueue, HoldsItsOwnCopyOfTheJob) {
  JobQueue<Job, 2> queue;
  Job mine{7, 11};
  EXPECT_TRUE(queue.push(mine));
  mine.id = 99;
  mine.payload = 99;
  Job out;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out.id, 7);
  EXPECT_EQ(out.payload, 11);
}

// No allocation on the hot path: the storage is the slots themselves. If a pointer or a container
// ever replaces the array, this fails.
TEST(JobQueue, StorageIsInline) {
  EXPECT_EQ(sizeof(JobQueue<Job, 4>), 4 * sizeof(Job) + 2 * sizeof(int));
  EXPECT_TRUE((std::is_trivially_copyable_v<JobQueue<Job, 2>>));
}

// ---- the submit / idle-exit policy --------------------------------------------------------

TEST(DomainPolicy, SubmitDecision) {
  EXPECT_EQ(submitAction(false, 0, 4), SubmitAction::SpawnThenAccept);  // first job creates the task
  EXPECT_EQ(submitAction(true, 0, 4), SubmitAction::Accept);            // task alive, room
  EXPECT_EQ(submitAction(true, 3, 4), SubmitAction::Accept);            // last free slot
  EXPECT_EQ(submitAction(true, 4, 4), SubmitAction::RefuseQueueFull);   // full
  EXPECT_EQ(submitAction(false, 4, 4), SubmitAction::RefuseQueueFull);  // full, and no task either
}

TEST(DomainPolicy, OnlyAnEmptyQueueLetsTheTaskGo) {
  EXPECT_TRUE(exitWhenIdle(0));
  EXPECT_FALSE(exitWhenIdle(1));
}

// ---- the failure path, end to end ---------------------------------------------------------

// A full queue returns false and the job stays with the caller: nothing is dropped, and nothing is
// left waiting for a completion that will not come. Once the task drains, the same submit succeeds.
TEST(DomainSimulation, FullQueueRefusesAndRecovers) {
  FakeDomain<2> domain;
  EXPECT_TRUE(domain.submit(Job{1, 0}));
  EXPECT_TRUE(domain.submit(Job{2, 0}));
  EXPECT_EQ(domain.queued(), 2);

  const Job refused{3, 0};
  EXPECT_FALSE(domain.submit(refused));
  EXPECT_EQ(refused.id, 3);  // still the caller's, untouched
  EXPECT_EQ(domain.refusals(), 1);
  EXPECT_EQ(domain.queued(), 2);

  EXPECT_EQ(domain.drain(), (std::vector<int>{1, 2}));
  EXPECT_EQ(domain.queued(), 0);
  EXPECT_TRUE(domain.submit(refused));
  EXPECT_EQ(domain.drain(), (std::vector<int>{3}));
}

// Lazy spawn then self-idle: the task exists only while there is work, and the next job brings it
// back. Every job is run exactly once across the exit/respawn.
TEST(DomainSimulation, TaskExitsWhenIdleAndComesBackForTheNextJob) {
  FakeDomain<4> domain;
  EXPECT_FALSE(domain.taskAlive());
  EXPECT_TRUE(domain.submit(Job{1, 0}));
  EXPECT_TRUE(domain.taskAlive());
  EXPECT_EQ(domain.spawned(), 1);
  EXPECT_EQ(domain.drain(), (std::vector<int>{1}));
  EXPECT_FALSE(domain.taskAlive());  // drained and idle: the task deleted itself

  EXPECT_TRUE(domain.submit(Job{2, 0}));
  EXPECT_EQ(domain.spawned(), 2);  // a new task for the next job
  EXPECT_EQ(domain.drain(), (std::vector<int>{2}));
}

// Nothing is lost across an exit: a job queued while the task is alive is still run, and a job
// queued after it exits is run by the replacement.
TEST(DomainSimulation, NoJobIsLostAcrossAnIdleExit) {
  FakeDomain<4> domain;
  std::vector<int> ran;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(domain.submit(Job{i, 0})) << "job " << i;
    if (i % 3 == 2) {  // the task wakes, drains, idles, exits
      const auto now = domain.drain();
      ran.insert(ran.end(), now.begin(), now.end());
    }
  }
  const auto rest = domain.drain();
  ran.insert(ran.end(), rest.begin(), rest.end());
  ASSERT_EQ(ran.size(), 10u);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(ran[static_cast<size_t>(i)], i);
  EXPECT_EQ(domain.refusals(), 0);
}

// ---- what the reader does with a refusal ---------------------------------------------------

namespace {

std::string readSource(const char* rel) {
  std::ifstream in(std::string(CP_REPO_ROOT) + "/" + rel);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// The reader must handle a refused job: the snapshot goes back to pending (retried on the next
// idle frame) and the exit path writes it inline rather than waiting for a completion that will
// never come. (The store half of this is proved in test/progress_saves; this pins the call site,
// because the reader has no host seam for its own methods.)
TEST(ReaderCallSite, RefusedProgressJobIsRetriedOrWrittenInline) {
  const auto src = readSource("src/activities/reader/EpubReaderActivity.cpp");
  const auto submit = src.find("ProgressWriter::get().submit(job)");
  ASSERT_NE(submit, std::string::npos) << "nothing submits to the progress writer any more";
  const auto release = src.find("store.release(handoff.slot, handoff.token)", submit);
  ASSERT_NE(release, std::string::npos) << "a refused job is not put back";
  EXPECT_LT(release - submit, 400u) << "the release is not in the refusal path";
  const auto inlineWrite = src.find("ProgressFile::write(cachePath, handoff.record)", release);
  ASSERT_NE(inlineWrite, std::string::npos) << "the exit path cannot write the refused position";
  EXPECT_LT(inlineWrite - release, 400u) << "the inline write is not in the refusal path";
}
