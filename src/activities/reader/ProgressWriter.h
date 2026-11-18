#pragma once

#include <cstdint>

#include "ReaderHandoff.h"
#include "ReaderTaskDomain.h"

// r163 C2 -- the progress writer: the one task domain this reader needs.
//
// The vendor runs five (main/application_tasks.cc:20-40). Our reader has exactly one piece of work
// that is both safe to run off the loop task and slow enough to matter: the per-page-turn progress
// write below. Nothing else qualifies today:
//
//   domain                 | why it is not a task here
//   -----------------------|-------------------------------------------------------------
//   chapter build / layout | the parser holds a Section the render task paints from, and its heap
//                          | peak is the reason makeRoomForChapterBuild() stops the radio; moving
//                          | it would need the section ownership rework the trace marks as the
//                          | prefetch's prerequisite, not a task-domain change.
//   glyph prewarm          | rasterises into the font cache and reads the renderer: a worker
//                          | callback must not do either (ReaderHandoff.h rule 1).
//   image decode           | borrows the framebuffer (FrameBufferLoan), which is the render task's.
//   bookkeeping/UI         | microseconds long; a task hop would cost more than the work.
//
// Numbers, and why they are not the vendor's 32/4/16/16/24:
//   * queue depth 2 -- a page turn can be accepted while the previous write is still on the card,
//     and that is the whole overlap this domain can see. Every job is a fixed sub-100-byte value held
//     inline, so depth 2 costs under 200 bytes of .bss; the vendor's 32 is sized for a domain of file
//     copies, and 32 of ours would only hide a stuck consumer behind 3.5 KB.
//   * completions 4 -- at least the queue depth, so a completion can never be refused and lost.
//   * stack 3072 -- the job body is ProgressFile::write(): two std::string path concatenations and
//     an SdFat write/rename. ble-start runs on 3072, the render task on 8192 because it parses;
//     3072 is one KB above the deepest path here and leaves the canary room.
//   * priority 1 -- the same as the loop and render tasks. A higher priority would let a progress
//     write preempt the compose it is meant to stay out of.
//   * idle exit 2000 ms -- the vendor's 2 s, kept: it is long enough that flipping pages does not
//     respawn the task between turns, and short enough that a reader left open has no extra task.

namespace readerprogress {

// Everything the write needs, by value: the cache path is copied in, not referenced, so the job is
// safe to run after the Epub object (or the reader) is gone.
struct WriteJob {
  static constexpr size_t kPathMax = 72;
  char cachePath[kPathMax] = {};
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
  // The session the position belongs to, echoed back on the completion (ReaderHandoff.h rule 2).
  uint32_t generation = 0;
  // The pending snapshot this write settles: its token, and the store slot that owns it.
  uint32_t token = 0;
  int slot = -1;

  void run() const;
};

// What the UI sees: a failure to report, stamped with the session it belongs to. The store's own
// bookkeeping is settled by the writer task itself (ProgressSaves::complete), so a result from a
// previous session can be dropped without stranding the pending record.
struct WriteDone {
  bool written = false;
  uint32_t generation = 0;
  uint32_t token = 0;
};

class ProgressWriter {
 public:
  static constexpr int kQueueDepth = 2;
  static constexpr int kCompletions = 4;

  static ProgressWriter& get();

  // False means NOT accepted: the queue is full or the task could not be created. The caller keeps
  // the job and writes it on its own task instead -- the reader never waits for a slot.
  bool submit(const WriteJob& job);
  // The oldest completion from `generation`, or false. Completions from an older session are
  // dropped here, not applied.
  bool poll(const uint32_t generation, WriteDone& out);

  int queued() { return domain_.queued(); }
  int spawnedTasks() { return domain_.spawnedTasks(); }

  ProgressWriter(const ProgressWriter&) = delete;
  ProgressWriter& operator=(const ProgressWriter&) = delete;

 private:
  ProgressWriter();
  friend struct WriteJob;
  // Called by WriteJob::run() on the domain's task. The slot set is sized to the queue depth, so
  // this cannot be refused; it is checked anyway, because a refused completion would be a lost one.
  void publish(const WriteDone& done);

  readertasks::Domain<WriteJob, kQueueDepth> domain_;
  readerhandoff::CompletionSlots<WriteDone, kCompletions> completions_;
};

}  // namespace readerprogress
