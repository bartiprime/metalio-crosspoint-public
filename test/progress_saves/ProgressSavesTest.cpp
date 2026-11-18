#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

#include "activities/reader/ProgressSaves.h"

using readerprogress::ProgressSaves;
using progressrecord::Record;

namespace {

constexpr const char* kBookA = "/.crosspoint/epub_1111";
constexpr const char* kBookB = "/.crosspoint/epub_2222";

Record at(const uint16_t spine, const uint16_t page, const uint16_t total = 0) {
  Record record;
  record.spineIndex = spine;
  record.pageNumber = page;
  record.pageCount = total;
  return record;
}

}  // namespace

// ---- the coalescing rule ------------------------------------------------------------------

TEST(ProgressSaves, AcceptsAndPeersTheLatestPosition) {
  ProgressSaves saves;
  Record out;
  EXPECT_FALSE(saves.peek(kBookA, out));  // nothing accepted yet
  EXPECT_TRUE(saves.accept(kBookA, at(2, 10), 7, 100));
  ASSERT_TRUE(saves.peek(kBookA, out));
  EXPECT_EQ(out.spineIndex, 2);
  EXPECT_EQ(out.pageNumber, 10);
  EXPECT_TRUE(saves.dirty(kBookA));
}

// Ten page turns before the write lands are one pending position: the tenth.
TEST(ProgressSaves, SavesMergeToTheLatestRatherThanQueueing) {
  ProgressSaves saves;
  for (int page = 0; page < 10; ++page) {
    EXPECT_TRUE(saves.accept(kBookA, at(1, static_cast<uint16_t>(page)), 7, 100 + page));
  }
  EXPECT_EQ(saves.pendingBooks(), 1);  // one book, one record
  Record out;
  ASSERT_TRUE(saves.peek(kBookA, out));
  EXPECT_EQ(out.pageNumber, 9);

  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take(kBookA, handoff));
  EXPECT_EQ(handoff.record.pageNumber, 9);
  EXPECT_FALSE(saves.take(kBookA, handoff));  // nothing else queued behind it
}

// ---- the writer handoff -------------------------------------------------------------------

TEST(ProgressSaves, TakeMarksInFlightAndCompleteClearsIt) {
  ProgressSaves saves;
  saves.accept(kBookA, at(3, 4), 1, 10);
  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take(kBookA, handoff));
  EXPECT_TRUE(saves.inFlight(kBookA));
  EXPECT_TRUE(saves.dirty(kBookA));  // in flight is still not on the card

  saves.complete(handoff.slot, handoff.token, /*written=*/true);
  EXPECT_FALSE(saves.inFlight(kBookA));
  EXPECT_FALSE(saves.dirty(kBookA));
}

TEST(ProgressSaves, FailedWriteStaysPending) {
  ProgressSaves saves;
  saves.accept(kBookA, at(3, 4), 1, 10);
  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take(kBookA, handoff));
  saves.complete(handoff.slot, handoff.token, /*written=*/false);
  EXPECT_FALSE(saves.inFlight(kBookA));
  EXPECT_TRUE(saves.dirty(kBookA));  // retried, not dropped
  Record out;
  ASSERT_TRUE(saves.peek(kBookA, out));
  EXPECT_EQ(out.pageNumber, 4);
}

// The writer refused the job: the snapshot goes back to pending, unchanged.
TEST(ProgressSaves, ReleasePutsTheSnapshotBack) {
  ProgressSaves saves;
  saves.accept(kBookA, at(3, 4), 1, 10);
  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take(kBookA, handoff));
  saves.release(handoff.slot, handoff.token);
  EXPECT_FALSE(saves.inFlight(kBookA));
  EXPECT_TRUE(saves.dirty(kBookA));
  ProgressSaves::Handoff again;
  ASSERT_TRUE(saves.take(kBookA, again));
  EXPECT_EQ(again.record.pageNumber, 4);
}

// A page turn during the write is not lost: the book stays dirty and the NEWER position is written
// next. The completion of the older write must not mark it clean.
TEST(ProgressSaves, AcceptDuringFlightKeepsTheBookDirty) {
  ProgressSaves saves;
  saves.accept(kBookA, at(1, 5), 1, 10);
  ProgressSaves::Handoff first;
  ASSERT_TRUE(saves.take(kBookA, first));

  EXPECT_TRUE(saves.accept(kBookA, at(1, 6), 1, 20));  // the reader turned while writing
  saves.complete(first.slot, first.token, /*written=*/true);
  EXPECT_FALSE(saves.inFlight(kBookA));
  EXPECT_TRUE(saves.dirty(kBookA));

  ProgressSaves::Handoff second;
  ASSERT_TRUE(saves.take(kBookA, second));
  EXPECT_EQ(second.record.pageNumber, 6);
  saves.complete(second.slot, second.token, /*written=*/true);
  EXPECT_FALSE(saves.dirty(kBookA));
}

// A completion that names a snapshot which is no longer the slot's is ignored: two writers cannot
// clear a record they do not own.
TEST(ProgressSaves, StaleCompletionCannotClearAPendingRecord) {
  ProgressSaves saves;
  saves.accept(kBookA, at(1, 5), 1, 10);
  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take(kBookA, handoff));

  saves.complete(handoff.slot, handoff.token + 1000, /*written=*/true);  // not this snapshot
  EXPECT_TRUE(saves.dirty(kBookA));
}

// ---- one record per book ------------------------------------------------------------------

TEST(ProgressSaves, BooksDoNotShareSlots) {
  ProgressSaves saves;
  saves.accept(kBookA, at(1, 5), 1, 10);
  saves.accept(kBookB, at(9, 99), 2, 11);
  EXPECT_EQ(saves.pendingBooks(), 2);

  Record out;
  ASSERT_TRUE(saves.peek(kBookA, out));
  EXPECT_EQ(out.pageNumber, 5);
  ASSERT_TRUE(saves.peek(kBookB, out));
  EXPECT_EQ(out.pageNumber, 99);

  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take(kBookA, handoff));
  EXPECT_EQ(handoff.record.pageNumber, 5);
  EXPECT_TRUE(saves.dirty(kBookB));  // untouched by A's write
}

// ---- bounded store behaviour --------------------------------------------------------------

TEST(ProgressSaves, RefusesWhenEverySlotIsWriting) {
  ProgressSaves saves;
  char path[64];
  ProgressSaves::Handoff handoff;
  for (int i = 0; i < ProgressSaves::kBooks; ++i) {
    std::snprintf(path, sizeof(path), "/.crosspoint/epub_%d", i);
    ASSERT_TRUE(saves.accept(path, at(1, static_cast<uint16_t>(i)), 1, 10));
    ASSERT_TRUE(saves.take(path, handoff));  // all in flight
  }
  EXPECT_FALSE(saves.accept("/.crosspoint/epub_new", at(1, 0), 1, 99));  // a visible refusal
}

// Eviction happens only between CLEAN slots: the four books here have all been written, so their
// positions are on the card and reusing the quietest one loses nothing.
TEST(ProgressSaves, EvictsTheQuietestBookWhosePositionIsOnTheCard) {
  ProgressSaves saves;
  char path[64];
  ProgressSaves::Handoff handoff;
  for (int i = 0; i < ProgressSaves::kBooks; ++i) {
    std::snprintf(path, sizeof(path), "/.crosspoint/epub_%d", i);
    ASSERT_TRUE(saves.accept(path, at(1, static_cast<uint16_t>(i)), 1, 10 + i));
    ASSERT_TRUE(saves.take(path, handoff));
    saves.complete(handoff.slot, handoff.token, /*written=*/true);
    EXPECT_FALSE(saves.dirty(path));
  }
  // The oldest (epub_0) is clean and not in flight, so a fifth book reuses its slot.
  EXPECT_TRUE(saves.accept("/.crosspoint/epub_new", at(2, 1), 1, 99));
  Record out;
  EXPECT_FALSE(saves.peek("/.crosspoint/epub_0", out));
  ASSERT_TRUE(saves.peek("/.crosspoint/epub_new", out));
  EXPECT_EQ(out.pageNumber, 1);
}

// A slot that is accepted but NOT on the card is a position the reader was told was remembered.
// Reusing it to make room for another book would drop that position silently, so the store refuses
// instead -- the caller then sees a false and must not believe the page was saved.
TEST(ProgressSaves, NeverEvictsADirtySlot) {
  ProgressSaves saves;
  char path[64];
  for (int i = 0; i < ProgressSaves::kBooks; ++i) {
    std::snprintf(path, sizeof(path), "/.crosspoint/epub_%d", i);
    ASSERT_TRUE(saves.accept(path, at(1, static_cast<uint16_t>(i)), 1, 10 + i));
  }
  EXPECT_TRUE(saves.dirty("/.crosspoint/epub_0"));  // accepted, nothing written yet
  EXPECT_FALSE(saves.accept("/.crosspoint/epub_new", at(2, 1), 1, 99))
      << "four pending books: a fifth accept is a visible refusal, not a silent eviction";
  Record out;
  ASSERT_TRUE(saves.peek("/.crosspoint/epub_0", out)) << "the quietest pending book kept its position";
  EXPECT_EQ(out.pageNumber, 0);

  // Once a book's position IS on the card, its slot is reusable: nothing is lost by reusing it.
  ProgressSaves::Handoff handoff;
  ASSERT_TRUE(saves.take("/.crosspoint/epub_0", handoff));
  saves.complete(handoff.slot, handoff.token, /*written=*/true);
  EXPECT_FALSE(saves.dirty("/.crosspoint/epub_0"));
  EXPECT_TRUE(saves.accept("/.crosspoint/epub_new", at(2, 1), 1, 99));
  EXPECT_FALSE(saves.peek("/.crosspoint/epub_0", out));
  ASSERT_TRUE(saves.peek("/.crosspoint/epub_new", out));
  EXPECT_EQ(out.pageNumber, 1);
}

// ---- the record itself -------------------------------------------------------------------

TEST(ProgressRecord, RoundTripsEveryLengthTheLoaderAccepts) {
  Record original;
  original.spineIndex = 0x1234;
  original.pageNumber = 0x5678;
  original.pageCount = 0x9ABC;
  original.visibleTextOffset = 0xDEADBEEF;
  original.hasVisibleTextOffset = true;

  uint8_t bytes[10];
  ASSERT_EQ(progressrecord::encode(original, bytes), 10u);
  Record decoded;
  ASSERT_TRUE(progressrecord::decode(bytes, 10, decoded));
  EXPECT_EQ(decoded, original);

  // Without the offset the record is 6 bytes and decodes with the flag cleared (the offset itself
  // is not carried, so it reads back as 0).
  original.hasVisibleTextOffset = false;
  original.visibleTextOffset = 0;
  ASSERT_EQ(progressrecord::encode(original, bytes), 6u);
  ASSERT_TRUE(progressrecord::decode(bytes, 6, decoded));
  EXPECT_EQ(decoded, original);

  // The legacy 4-byte record carries only spine and page.
  ASSERT_TRUE(progressrecord::decode(bytes, 4, decoded));
  EXPECT_EQ(decoded.spineIndex, 0x1234);
  EXPECT_EQ(decoded.pageNumber, 0x5678);
  EXPECT_FALSE(decoded.hasVisibleTextOffset);

  EXPECT_FALSE(progressrecord::decode(bytes, 5, decoded));
}

// ---- the destructor rule ------------------------------------------------------------------

namespace {

std::string readSource(const char* rel) {
  std::ifstream in(std::string(CP_REPO_ROOT) + "/" + rel);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// "A destructor must never save a stale position": the reader's destructor flushes nothing and
// derives nothing. The accepted snapshot is flushed by onExit()/prepareForSleep(), before the
// section is torn down. This pins that, because the rule is invisible in the type system.
TEST(ProgressReopenRule, PendingSnapshotIsConsultedBeforeTheFile) {
  const auto src = readSource("src/activities/reader/EpubReaderActivity.cpp");
  const auto loadBook = src.find("bool EpubReaderActivity::loadBook()");
  ASSERT_NE(loadBook, std::string::npos);
  const auto peek = src.find("ProgressStore::get().peek(", loadBook);
  const auto file = src.find("/progress.bin", loadBook);
  ASSERT_NE(peek, std::string::npos) << "loadBook does not consult the pending snapshot";
  ASSERT_NE(file, std::string::npos);
  EXPECT_LT(peek, file) << "the file is read before the newer pending snapshot";
}

TEST(ProgressDestructorRule, ReaderDestructorNeverSaves) {
  const auto src = readSource("src/activities/reader/EpubReaderActivity.cpp");
  const auto start = src.find("EpubReaderActivity::~EpubReaderActivity()");
  ASSERT_NE(start, std::string::npos);
  const auto end = src.find("\nbool EpubReaderActivity::loadBook()", start);
  ASSERT_NE(end, std::string::npos);
  const auto body = src.substr(start, end - start);
  EXPECT_EQ(body.find("saveProgress("), std::string::npos) << "the destructor saves a position";
  EXPECT_EQ(body.find("saveProgressNow("), std::string::npos) << "the destructor saves a position";
  EXPECT_EQ(body.find("flushAcceptedProgress"), std::string::npos) << "the destructor flushes";
  EXPECT_EQ(body.find("ProgressFile::write"), std::string::npos) << "the destructor writes";
}
