#include "ProgressWriter.h"

#include <Logging.h>

#include "ProgressFile.h"
#include "ProgressStore.h"

namespace readerprogress {

void WriteJob::run() const {
  const bool written = ProgressFile::write(cachePath, progressrecord::Record{spineIndex, pageNumber, pageCount,
                                                                           visibleTextOffset, hasVisibleTextOffset});
  // Settle the pending snapshot first, whatever this task's session is: the store is book-keyed,
  // not session-keyed, and a record left marked dirty would block that book's next write forever.
  ProgressStore::get().complete(slot, token, written);
  if (!written) {
    // A write failure is reported through the completion as well: the reader paints
    // STR_SAVE_PROGRESS_FAILED when it polls it, and drops it if that session is gone.
    LOG_ERR("PRG", "Background progress write failed for spine %u page %u", spineIndex, pageNumber);
  }
  ProgressWriter::get().publish(WriteDone{written, generation, token});
}

ProgressWriter::ProgressWriter() {
  readertasks::Domain<WriteJob, kQueueDepth>::Config config;
  config.name = "reader-progress";
  config.stackBytes = 3072;
  config.priority = 1;
  config.idleExitMs = 2000;
  domain_.begin(config);
}

ProgressWriter& ProgressWriter::get() {
  static ProgressWriter instance;
  return instance;
}

bool ProgressWriter::submit(const WriteJob& job) { return domain_.submit(job); }

void ProgressWriter::publish(const WriteDone& done) {
  if (!completions_.publish(done.generation, done)) {
    // Cannot happen while kCompletions >= kQueueDepth: at most one completion per in-flight job.
    // If the sizes are ever changed apart, this is the line that says so.
    LOG_ERR("PRG", "Progress completion slots full; a write result was not advertised");
  }
}

bool ProgressWriter::poll(const uint32_t generation, WriteDone& out) { return completions_.poll(generation, out); }

}  // namespace readerprogress
