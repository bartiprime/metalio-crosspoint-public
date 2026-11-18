#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryIndexFile.h"
#include "LibraryPartial.h"
#include "LibraryText.h"
#include <string_view>

using namespace library;

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";

std::string numbered(const char* prefix, const unsigned value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s%04u", prefix, value);
  return text;
}

std::string pathAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return {};
  ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return {};
  std::string path;
  return index.readPath(record, path) ? path : std::string();
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  BuildStats stats;

  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    fake::add("/a.epub");
    fake::add("/b.epub");
  }

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true)); }
};

}  // namespace

TEST(LibraryBookFileTest, AcceptsBookFilesTheWalkLists) {
  EXPECT_TRUE(isLibraryBookFile("book.epub"));
  EXPECT_TRUE(isLibraryBookFile("/Books/book.epub"));
  EXPECT_TRUE(isLibraryBookFile("/notes.txt"));
  EXPECT_TRUE(isLibraryBookFile("/a/b/c/readme.md"));
  EXPECT_TRUE(isLibraryBookFile("/Comics/issue.xtc"));
  EXPECT_TRUE(isLibraryBookFile("/Comics/issue.xtch"));
  EXPECT_TRUE(isLibraryBookFile("/Books/./book.epub"));
}

TEST(LibraryBookFileTest, RejectsOtherFilesAndHiddenNames) {
  EXPECT_FALSE(isLibraryBookFile(""));
  EXPECT_FALSE(isLibraryBookFile("/Books/book.pdf"));
  EXPECT_FALSE(isLibraryBookFile("/Books/book.epub.part"));
  EXPECT_FALSE(isLibraryBookFile("/.crosspoint/library.dirty"));
  EXPECT_FALSE(isLibraryBookFile("/.crosspoint/plugins/foo/README.md"));
  EXPECT_FALSE(isLibraryBookFile("/.plugins/foo/notes.txt"));
  EXPECT_FALSE(isLibraryBookFile("/Books/.hidden/book.epub"));
  EXPECT_FALSE(isLibraryBookFile("/Books/._book.epub"));
  EXPECT_FALSE(isLibraryBookFile("._book.epub"));
}

TEST_F(LibraryBuilderTest, XtcTitleAndAuthorComeFromTheFile) {
  fake::add("/Comics/one.xtc");
  fake::add("/Comics/two.xtch");
  bookMetadata["/Comics/one.xtc"] = {"Go Problems", "Cho Chikun", true};
  bookMetadata["/Comics/two.xtch"] = {"Sakata no Go", "Sakata Eio", true};
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.header().bookCount, 4u);
  bool sawXtc = false;
  bool sawXtch = false;
  for (uint16_t ordinal = 0; ordinal < index.header().bookCount; ordinal++) {
    ClixRecord record{};
    std::string path;
    std::string title;
    std::string author;
    ASSERT_TRUE(index.readRecord(ordinal, record));
    ASSERT_TRUE(index.readPath(record, path));
    if (path != "/Comics/one.xtc" && path != "/Comics/two.xtch") continue;
    ASSERT_TRUE(index.readTitle(record, title));
    ASSERT_TRUE(index.readAuthor(record, author));
    EXPECT_EQ(title, bookMetadata[path].title);
    EXPECT_EQ(author, bookMetadata[path].author);
    (path == "/Comics/one.xtc" ? sawXtc : sawXtch) = true;
  }
  EXPECT_TRUE(sawXtc);
  EXPECT_TRUE(sawXtch);
}

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, FolderHeavyUnchangedReconciliationIoScalesLinearly) {
  const auto measure = [this](const unsigned count) {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < count; i++) {
      fake::add("/folder" + numbered("", i) + "/book.txt");
    }
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "unchanged build failed for " << count << " books";
      return 0u;
    }
    EXPECT_EQ(stats.metadataReused, count);
    EXPECT_FALSE(stats.indexReplaced);
    return fake::reads + fake::seeks;
  };

  const unsigned smallIo = measure(128);
  const unsigned largeIo = measure(256);
  EXPECT_LT(largeIo, smallIo * 3u);
}

TEST_F(LibraryBuilderTest, DirectoryEntriesAreEnumeratedOnce) {
  fake::add("/folder/c.txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, DirectoryResumeFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::add("/aa-folder/c.txt");
  fake::failDirectorySeek = true;

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
}

TEST_F(LibraryBuilderTest, RebuildVotesFromSourceAuthorInsteadOfPriorCanonicalAuthor) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Victor Hugo";
  bookMetadata["/b.epub"].author = "Hugo Victor";
  bookMetadata["/c.epub"].author = "Hugo Victor";
  initial();
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  std::string author;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "Victor Hugo");
}

TEST_F(LibraryBuilderTest, EqualBasenamesInDifferentFoldersReconcileIndependently) {
  fake::add("/one/same.epub");
  fake::add("/two/same.epub");
  bookMetadata["/one/same.epub"].title = "One";
  bookMetadata["/two/same.epub"].title = "Two";
  initial();
  fake::files["/two/same.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, ArrivalOrderFollowsModificationTimeOverDiscoveryOrder) {
  // a and b exist with the default time; c lands with an older timestamp and d
  // with the newest, so file times, not walk or firstSeen order, decide.
  fake::add("/c.epub", "book c", /*time=*/0);
  fake::add("/d.epub", "book d", /*time=*/9);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/d.epub");
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failAlloc = 3;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, TruncatedPersistedPathHashAbortsAndRetainsTheLiveIndex) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord record{};
  std::memcpy(&record, bytes.data() + recordOffset(header, 0), sizeof(record));
  record.nameOff = header.nameLen - 4;
  std::memcpy(bytes.data() + recordOffset(header, 0), &record, sizeof(record));
  const auto corrupted = bytes;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, corrupted);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, LibrariesPastOldGateAndAtFormatCeilingKeepAllOrders) {
  for (const unsigned count : {513u, static_cast<unsigned>(CLIX_MAX_RECORDS)}) {
    fake::reset();
    bookMetadata.clear();
    std::vector<unsigned> authorOrder(count);
    std::iota(authorOrder.begin(), authorOrder.end(), 0u);
    for (unsigned i = 0; i < count; i++) {
      const std::string path = "/book" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].title = numbered("Title ", count - 1 - i);
      bookMetadata[path].author = numbered("Writer ", (i * (count == 513 ? 257u : 2053u)) % count);
    }

    ASSERT_TRUE(buildLibraryIndex("/", stats, true)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == CLIX_MAX_RECORDS) {
      const auto old = fake::files[INDEX]->bytes;
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true));
      EXPECT_EQ(fake::parses, 0u);
      EXPECT_EQ(stats.metadataReused, CLIX_MAX_RECORDS);
      EXPECT_FALSE(stats.indexReplaced);
      EXPECT_EQ(fake::files[INDEX]->bytes, old);
      EXPECT_LT(fake::delays, 10000u);
    }

    std::sort(authorOrder.begin(), authorOrder.end(), [count](const unsigned a, const unsigned b) {
      return (a * (count == 513 ? 257u : 2053u)) % count < (b * (count == 513 ? 257u : 2053u)) % count;
    });
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t row = 0; row < count; row++) {
      EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, row), "/book" + numbered("", count - 1 - row) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
    }
  }
}

TEST_F(LibraryBuilderTest, SortAllocationFailureProducesValidDegradedIndex) {
  fake::reset();
  for (unsigned i = 0; i < 513; i++) fake::add("/book" + numbered("", i) + ".txt");
  // LIB1: the staging writer is heap-allocated now (the build outlives one call),
  // so the SORT KEY array is the seventh counted allocation instead of the sixth.
  fake::failAlloc = 7;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_TRUE(stats.ranksDegraded);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 513);
}


// --- LIB1: the stepper, the journal, and cancel = keep completed -------------

namespace {

constexpr char PART[] = "/.crosspoint/library.part";

// Drive the stepper until `books` records are durable, then cancel it. Returns
// the number of Progress steps actually taken.
int driveAndCancel(const unsigned books, BuildProgress* outProgress = nullptr) {
  LibraryIndexStepper stepper;
  EXPECT_TRUE(stepper.begin("/", true));
  int steps = 0;
  BuildStep step = BuildStep::Progress;
  while (step == BuildStep::Progress || step == BuildStep::Finalizing) {
    step = stepper.step();
    if (step == BuildStep::Progress && ++steps >= static_cast<int>(books)) break;
  }
  stepper.requestCancel();
  const BuildStep cancelled = stepper.step();
  EXPECT_EQ(cancelled, BuildStep::Cancelled) << "cancel must be observed at a book boundary";
  if (outProgress != nullptr) *outProgress = stepper.progress();
  return steps;
}

// Ten books in three folders, so the journal has both frame types and folder ids
// have to survive the resume.
void tenBooks() {
  for (unsigned i = 0; i < 4; i++) fake::add("/folder0/book" + numbered("", i) + ".epub");
  for (unsigned i = 0; i < 3; i++) fake::add("/folder1/book" + numbered("", i) + ".epub");
  for (unsigned i = 0; i < 3; i++) fake::add("/folder2/book" + numbered("", i) + ".epub");
}

}  // namespace

TEST(LibraryIndexStepperTest, CancelKeepsCompletedBooksAndLeavesTheLiveIndexAlone) {
  fake::reset();
  fake::add("/a.epub");
  fake::add("/b.epub");
  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  const auto before = fake::files[INDEX]->bytes;

  for (unsigned i = 0; i < 5; i++) fake::add("/new" + numbered("", i) + ".epub");

  BuildProgress progress;
  const int steps = driveAndCancel(2, &progress);
  EXPECT_EQ(steps, 2);
  EXPECT_EQ(progress.books, 2);

  // The partial is a side file, never the served index.
  ASSERT_TRUE(Storage.exists(PART));
  PartialSummary part;
  ASSERT_TRUE(readLibraryPartialSummary(part));
  EXPECT_EQ(part.records, 2);
  EXPECT_EQ(part.total, 0) << "M is unknown until the walk reaches the end of the card";

  // Nothing promoted: the old index is byte-identical.
  EXPECT_EQ(fake::files[INDEX]->bytes, before);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST(LibraryIndexStepperTest, ResumeFromPartialTwoOfTenContinuesAtBookThreeAndMatchesACleanRun) {
  fake::reset();
  tenBooks();

  // Reference: one uninterrupted walk on this exact card.
  BuildStats cleanStats;
  ASSERT_TRUE(buildLibraryIndex("/", cleanStats, true));
  ASSERT_EQ(cleanStats.books, 10);
  const auto clean = fake::files[INDEX]->bytes;
  ASSERT_TRUE(Storage.remove(INDEX));

  // Pause after two books, then resume. (The reference build above parsed
  // every book once, so the parse counter is cleared before the paused run.)
  fake::parses = 0;
  BuildProgress paused;
  driveAndCancel(2, &paused);
  EXPECT_EQ(paused.books, 2);
  EXPECT_EQ(fake::parses, 2u);

  fake::parses = 0;
  LibraryIndexStepper stepper;
  ASSERT_TRUE(stepper.begin("/", true));
  EXPECT_EQ(stepper.progress().resumed, 2) << "begin() must carry the journal prefix forward";
  BuildStep step = BuildStep::Progress;
  while (step == BuildStep::Progress || step == BuildStep::Finalizing) step = stepper.step();
  ASSERT_EQ(step, BuildStep::Done);

  // Continued at book three: the eight remaining books were parsed exactly once
  // each, and the two committed ones were not read again.
  EXPECT_EQ(fake::parses, 8u);
  EXPECT_EQ(stepper.progress().total, 10);
  EXPECT_EQ(stepper.stats().books, 10);

  // Promoted index equals a clean full run byte for byte.
  EXPECT_EQ(fake::files[INDEX]->bytes, clean);
  EXPECT_FALSE(Storage.exists(PART)) << "a promoted journal has no further use";
}

TEST(LibraryIndexStepperTest, TornTailIsTruncatedAtOpenAndTheRunStillCompletes) {
  fake::reset();
  tenBooks();
  BuildStats cleanStats;
  ASSERT_TRUE(buildLibraryIndex("/", cleanStats, true));
  const auto clean = fake::files[INDEX]->bytes;
  ASSERT_TRUE(Storage.remove(INDEX));

  driveAndCancel(2);
  ASSERT_TRUE(Storage.exists(PART));
  auto& partBytes = fake::files[PART]->bytes;
  const size_t whole = partBytes.size();
  // Twenty bytes of a frame that never finished: the shape of a pull during a
  // write. OPEN must cut back to the last frame boundary, not resume into it.
  for (int i = 0; i < 20; i++) partBytes.push_back(0xAA);
  ASSERT_EQ(fake::files[PART]->bytes.size(), whole + 20);

  PartialSummary truncated;
  ASSERT_TRUE(readLibraryPartialSummary(truncated));
  EXPECT_EQ(truncated.records, 2) << "the durable count comes from the frames, not the header";

  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 10);
  EXPECT_EQ(fake::files[INDEX]->bytes, clean);
  EXPECT_LT(fake::files[PART] ? fake::files[PART]->bytes.size() : 0u, whole + 20);
}

TEST(LibraryIndexStepperTest, ARecordCutInHalfLosesOnlyThatBook) {
  fake::reset();
  tenBooks();
  BuildStats cleanStats;
  ASSERT_TRUE(buildLibraryIndex("/", cleanStats, true));
  const auto clean = fake::files[INDEX]->bytes;
  ASSERT_TRUE(Storage.remove(INDEX));

  driveAndCancel(2);
  auto& partBytes = fake::files[PART]->bytes;
  // Cut the file in the middle of the last record's payload: one frame is gone,
  // the one before it must survive.
  partBytes.resize(partBytes.size() - PART_FRAME_HEADER_BYTES - 100);

  // The header still claims two: peek() reports the DURABLE count, and correcting
  // it is OPEN's job, which is what this test exercises through the stepper.
  fake::parses = 0;
  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 9u) << "book two is read again because its frame was torn";
  EXPECT_EQ(fake::files[INDEX]->bytes, clean);
}

TEST(LibraryIndexStepperTest, JournalIdentityMismatchDiscardsThePartial) {
  // At the journal itself, so every field of the identity is exercised: schema,
  // generation, format, fold version, metadata mode, card and root.
  fake::reset();
  tenBooks();
  driveAndCancel(2);
  ASSERT_TRUE(Storage.exists(PART));

  uint8_t payload[8] = {};
  PartIdentity base;
  base.generation = 2;
  base.baseGeneration = 1;
  base.cardId = 0x1234;
  base.metadataEnabled = true;
  base.rootPath = "/";

  {
    LibraryPartial journal;
    EXPECT_EQ(journal.open(PART, base, sizeof(payload)), PartOpen::Fresh);
    ASSERT_TRUE(journal.appendRecord(payload, sizeof(payload)));
    ASSERT_TRUE(journal.appendRecord(payload, sizeof(payload)));
    EXPECT_EQ(journal.records(), 2);
    journal.close();
  }
  {
    LibraryPartial journal;
    EXPECT_EQ(journal.open(PART, base, sizeof(payload)), PartOpen::Resumed);
    EXPECT_EQ(journal.records(), 2);
    journal.close();
  }

  const auto mismatch = [&](const char* what, const PartIdentity& identity) {
    LibraryPartial journal;
    EXPECT_EQ(journal.open(PART, identity, sizeof(payload)), PartOpen::Fresh) << what;
    EXPECT_EQ(journal.records(), 0) << what;
    journal.close();
    // Put the journal back for the next case.
    journal.open(PART, base, sizeof(payload));
    ASSERT_TRUE(journal.appendRecord(payload, sizeof(payload)));
    ASSERT_TRUE(journal.appendRecord(payload, sizeof(payload)));
    journal.close();
  };

  PartIdentity next = base;
  next.generation = 3;
  mismatch("generation", next);

  PartIdentity olderFormat = base;
  olderFormat.formatVersion = static_cast<uint8_t>(base.formatVersion + 1);
  mismatch("format version", olderFormat);

  PartIdentity olderFold = base;
  olderFold.foldVersion = static_cast<uint8_t>(base.foldVersion + 1);
  mismatch("fold version", olderFold);

  PartIdentity noMetadata = base;
  noMetadata.metadataEnabled = false;
  mismatch("metadata mode", noMetadata);

  PartIdentity otherRoot = base;
  otherRoot.rootPath = "/Books";
  mismatch("root path", otherRoot);

  PartIdentity otherCard = base;
  otherCard.cardId = 0x9999;
  mismatch("card identity", otherCard);
}

TEST(LibraryIndexStepperTest, CorruptJournalHeaderStartsAFullWalk) {
  fake::reset();
  tenBooks();
  driveAndCancel(2);
  auto& bytes = fake::files[PART]->bytes;
  bytes[8] = 0x7F;  // schema byte: unknown to this build

  LibraryIndexStepper stepper;
  ASSERT_TRUE(stepper.begin("/", true));
  EXPECT_EQ(stepper.progress().resumed, 0);
  BuildStep step = BuildStep::Progress;
  while (step == BuildStep::Progress || step == BuildStep::Finalizing) step = stepper.step();
  EXPECT_EQ(step, BuildStep::Done);
  EXPECT_EQ(stepper.stats().books, 10);
}

TEST(LibraryIndexStepperTest, AJournalThatFinishedItsWalkPromotesWithoutWalkingAgain) {
  // The interesting interruption is the one after the last book: the card was
  // read in full and the promote never happened. Re-walking to arrive at the same
  // records would cost minutes, so the journal's own "walk complete" decides.
  fake::reset();
  tenBooks();
  BuildStats cleanStats;
  ASSERT_TRUE(buildLibraryIndex("/", cleanStats, true));
  const auto clean = fake::files[INDEX]->bytes;
  ASSERT_TRUE(Storage.remove(INDEX));

  {
    LibraryIndexStepper stepper;
    ASSERT_TRUE(stepper.begin("/", true));
    BuildStep step = BuildStep::Progress;
    while (step == BuildStep::Progress) step = stepper.step();
    ASSERT_EQ(step, BuildStep::Finalizing);
    // Abandon it here: the destructor tears down the staging files but keeps the
    // journal, which is exactly the state a power cut would leave.
  }

  PartialSummary part;
  ASSERT_TRUE(readLibraryPartialSummary(part));
  EXPECT_EQ(part.total, 10);
  EXPECT_EQ(part.records, 10);

  fake::parses = 0;
  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 0u) << "a journal whose walk finished must not re-read the card";
  EXPECT_EQ(fake::files[INDEX]->bytes, clean);
  EXPECT_FALSE(Storage.exists(PART));
}

TEST(LibraryIndexStepperTest, IndexHeaderCarriesTheMonotonicGeneration) {
  fake::reset();
  tenBooks();
  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  IndexSummary first;
  ASSERT_TRUE(readLibraryIndexSummary(first));
  EXPECT_EQ(first.generation, 1u);
  EXPECT_EQ(first.books, 10);

  fake::files["/folder0/book0000.epub"]->time++;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  IndexSummary second;
  ASSERT_TRUE(readLibraryIndexSummary(second));
  EXPECT_EQ(second.generation, 2u) << "generation is a counter; a lost RTC makes age meaningless";
}

// --- LIB1-L2: the smart diff ------------------------------------------------

TEST_F(LibraryBuilderTest, SmartDiffCountsNewChangedMissingAndUnchanged) {
  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i < 10; i++) fake::add("/b" + numbered("", i) + ".epub");
  initial();
  ASSERT_EQ(stats.books, 10);
  ASSERT_EQ(stats.unchanged, 0) << "a first build has nothing to compare against";
  ASSERT_EQ(stats.added, 10);
  ASSERT_EQ(fake::parses, 10u);

  // The card changes between two walk in exactly the four known ways.
  // Distinct sizes on purpose: a new book that happens to weigh exactly what a
  // removed one did is treated as a RENAME, which is a different count.
  fake::add("/n0000.epub", "a brand new book here");
  fake::add("/n0001.epub", "another brand new book");
  fake::files["/b0000.epub"]->time++;             // changed: mtime moved
  fake::files["/b0001.epub"]->bytes.push_back('x');  // changed: size moved
  ASSERT_TRUE(Storage.remove("/b0002.epub"));     // missing
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.books, 11);
  EXPECT_EQ(stats.added, 2);
  EXPECT_EQ(stats.renamed, 0);
  EXPECT_EQ(stats.changed, 2);
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.unchanged, 7);
  // The whole win: only the 2 new and the 2 changed books were parsed. A rebuild
  // that parsed 11 would make the counts correct and the feature pointless.
  EXPECT_EQ(fake::parses, 4u);
  EXPECT_EQ(stats.parsed, 4);
  EXPECT_EQ(stats.metadataReused, 7);
  // Every book is accounted for exactly once.
  EXPECT_EQ(stats.unchanged + stats.changed + stats.added + stats.renamed, stats.books);
}

TEST_F(LibraryBuilderTest, LargeUnchangedRebuildParsesNoBookAtAll) {
  // 300 books, not 3: the per-book reuse path is what this pins, and a library
  // big enough to matter is the case the feature exists for.
  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i < 300; i++) {
    const std::string path = "/lib/b" + numbered("", i) + ".epub";
    fake::add(path);
    bookMetadata[path].title = numbered("Title ", i);
    bookMetadata[path].author = numbered("Writer ", i % 17);
  }
  initial();
  ASSERT_EQ(stats.books, 300);
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u) << "an unchanged book must never be opened";
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.unchanged, 300);
  EXPECT_EQ(stats.changed, 0);
  EXPECT_EQ(stats.added, 0);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old) << "nothing changed, so nothing is rewritten";
}

// --- LIB1-L2: series -------------------------------------------------------

namespace {

ClixRecord recordAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  ClixRecord out{};
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return out;
  index.readRecord(ordinal, out);
  return out;
}

std::string seriesAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const ClixRecord record = recordAt(index, order, row);
  std::string series;
  index.readSeries(record, series);
  return series;
}

}  // namespace

TEST_F(LibraryBuilderTest, SeriesSortsByDeclaredSeriesThenIndexWithStandaloneLast) {
  fake::reset();
  bookMetadata.clear();
  fake::add("/series/messiah.epub");
  fake::add("/series/children.epub");
  fake::add("/series/dune.epub");
  fake::add("/stand/stand.epub");
  fake::add("/stand/foundation.epub");
  bookMetadata["/series/dune.epub"] = {"Dune", "Frank Herbert", true, "Dune Chronicles", 10};
  bookMetadata["/series/messiah.epub"] = {"Dune Messiah", "Frank Herbert", true, "Dune Chronicles", 20};
  bookMetadata["/series/children.epub"] = {"Children of Dune", "Frank Herbert", true, "Dune Chronicles", 30};
  bookMetadata["/stand/stand.epub"] = {"The Stand", "Stephen King", true, "", 0};

  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), 5);
  EXPECT_EQ(stats.seriesBooks, 3);

  // SeriesAsc: the declared series by position, then the books that declared no
  // series as ONE run at the end.
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 0), "/series/dune.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 1), "/series/messiah.epub");
  EXPECT_EQ(pathAt(index, SortOrder::SeriesAsc, 2), "/series/children.epub");
  const std::string fourth = pathAt(index, SortOrder::SeriesAsc, 3);
  const std::string fifth = pathAt(index, SortOrder::SeriesAsc, 4);
  EXPECT_TRUE(fourth == "/stand/foundation.epub" || fourth == "/stand/stand.epub") << fourth;
  EXPECT_TRUE(fifth == "/stand/foundation.epub" || fifth == "/stand/stand.epub") << fifth;
  EXPECT_NE(fourth, fifth);

  EXPECT_EQ(seriesAt(index, SortOrder::SeriesAsc, 0), "Dune Chronicles");
  EXPECT_EQ(seriesAt(index, SortOrder::SeriesAsc, 3), "") << "a standalone has no series, and says so";
  EXPECT_EQ(seriesAt(index, SortOrder::SeriesAsc, 4), "");

  // Descending is the exact reverse, so the Standalone run heads the list.
  EXPECT_EQ(pathAt(index, SortOrder::SeriesDesc, 4), pathAt(index, SortOrder::SeriesAsc, 0));
  EXPECT_EQ(pathAt(index, SortOrder::SeriesDesc, 0), pathAt(index, SortOrder::SeriesAsc, 4));
  index.close();
}

TEST_F(LibraryBuilderTest, SeriesIsNeverDerivedFromTheTitle) {
  // "Foundation and Empire" reads like volume two of something. Its OPF says
  // nothing, so the shelf must show it as a Standalone rather than invent
  // "Foundation". A wrong guess is worse than a visible Standalone.
  fake::reset();
  bookMetadata.clear();
  fake::add("/world/foundation.epub");
  fake::add("/world/empire.epub");
  bookMetadata["/world/foundation.epub"].title = "Foundation";
  bookMetadata["/world/empire.epub"].title = "Foundation and Empire";
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  for (uint16_t row = 0; row < index.bookCount(); row++) {
    const ClixRecord record = recordAt(index, SortOrder::SeriesAsc, row);
    std::string series;
    index.readSeries(record, series);
    EXPECT_TRUE(series.empty()) << "guessed a series from the title: " << series;
    EXPECT_EQ(record.seriesIndex, 0);
    EXPECT_EQ(record.seriesFoldLen, 0);
  }
  EXPECT_EQ(stats.seriesBooks, 0);
  index.close();
}

TEST_F(LibraryBuilderTest, FractionalSeriesIndexSurvivesAsTenths) {
  fake::reset();
  bookMetadata.clear();
  fake::add("/s/one.epub");
  fake::add("/s/two-five.epub");
  bookMetadata["/s/one.epub"] = {"First", "A Writer", true, "A Series", 10};
  bookMetadata["/s/two-five.epub"] = {"Novella", "A Writer", true, "A Series", 25};
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(recordAt(index, SortOrder::SeriesAsc, 0).seriesIndex, 10);
  EXPECT_EQ(recordAt(index, SortOrder::SeriesAsc, 1).seriesIndex, 25);
  index.close();
}

// --- LIB1-L2: search -------------------------------------------------------

namespace {

// The Library screen's search, reduced to its data: one sequential pass over the
// record section, the four stored folds, and the ranking rule. Buckets are
// concatenated best-rank-first, so the order this returns IS the order the
// screen shows.
std::vector<std::string> searchLibrary(LibraryIndexFile& index, const std::string& query) {
  std::vector<std::string> buckets[6];
  const std::string needle = fold(query);
  ClixRecord batch[16];
  for (uint16_t base = 0; base < index.bookCount(); base += 16) {
    const uint16_t want = static_cast<uint16_t>(std::min<size_t>(16, index.bookCount() - base));
    if (!index.readRecords(base, batch, want)) return {};
    for (uint16_t i = 0; i < want; i++) {
      const SearchKeys keys{
          std::string_view(batch[i].fold, batch[i].foldLen),
          std::string_view(batch[i].authorFold, batch[i].authorFoldLen),
          std::string_view(batch[i].seriesFold, batch[i].seriesFoldLen),
          std::string_view(batch[i].nameFold, batch[i].nameFoldLen),
      };
      const MatchRank rank = rankSearchMatch(keys, needle);
      if (rank == MatchRank::None) continue;
      std::string path;
      if (!index.readPath(batch[i], path)) continue;
      buckets[static_cast<int>(rank)].push_back(path);
    }
  }
  std::vector<std::string> rows;
  for (int rank = static_cast<int>(MatchRank::TitlePrefix); rank >= 1; rank--) {
    rows.insert(rows.end(), buckets[rank].begin(), buckets[rank].end());
  }
  return rows;
}

}  // namespace

TEST_F(LibraryBuilderTest, SearchRanksPrefixTitleThenTitleThenSeriesThenAuthorThenFilename) {
  fake::reset();
  bookMetadata.clear();
  // Every book answers the query "dune" through exactly ONE field, so the order
  // below can only come from the ranking rule.
  fake::add("/rank/a-prefix.epub");
  fake::add("/rank/b-substring.epub");
  fake::add("/rank/c-series.epub");
  fake::add("/rank/d-author.epub");
  fake::add("/rank/dune005.epub");
  bookMetadata["/rank/a-prefix.epub"] = {"Dune", "Frank Herbert", true, "", 0};
  bookMetadata["/rank/b-substring.epub"] = {"Children of Dune", "Frank Herbert", true, "", 0};
  bookMetadata["/rank/c-series.epub"] = {"Sandworms", "Brian Herbert", true, "Dune Chronicles", 10};
  bookMetadata["/rank/d-author.epub"] = {"Wasteland", "Ada Dune", true, "", 0};
  bookMetadata["/rank/dune005.epub"] = {"Untitled Work", "Nobody", true, "", 0};

  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  const std::vector<std::string> hits = searchLibrary(index, "dune");
  ASSERT_EQ(hits.size(), 5u);
  EXPECT_EQ(hits[0], "/rank/a-prefix.epub") << "prefix of the title outranks everything";
  EXPECT_EQ(hits[1], "/rank/b-substring.epub") << "a title hit outranks series, author and filename";
  EXPECT_EQ(hits[2], "/rank/c-series.epub") << "a series hit outranks the author";
  EXPECT_EQ(hits[3], "/rank/d-author.epub") << "the author outranks the filename";
  EXPECT_EQ(hits[4], "/rank/dune005.epub") << "the filename is last but still findable";
  index.close();
}

TEST_F(LibraryBuilderTest, SearchIsCaseAndDiacriticInsensitiveOnEveryField) {
  fake::reset();
  bookMetadata.clear();
  fake::add("/acc/eneide.epub");
  fake::add("/acc/kierkegaard.epub");
  fake::add("/acc/edition.epub");
  fake::add("/acc/plain.epub");
  bookMetadata["/acc/eneide.epub"] = {"L'\u00C9n\u00E9ide", "Virgile", true, "", 0};
  bookMetadata["/acc/kierkegaard.epub"] = {"Fear and Trembling", "S\u00F8ren Kierkegaard", true, "", 0};
  bookMetadata["/acc/edition.epub"] = {"Collected", "Someone", true, "\u00C9dition Sp\u00E9ciale", 10};
  bookMetadata["/acc/plain.epub"] = {"A Book", "Another", true, "", 0};
  fake::add("/acc/\u00C4rger.epub");  // filename-only hit, precomposed A-diaeresis
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(searchLibrary(index, "eneide"), std::vector<std::string>{"/acc/eneide.epub"});
  EXPECT_EQ(searchLibrary(index, "SOREN"), std::vector<std::string>{"/acc/kierkegaard.epub"});
  EXPECT_EQ(searchLibrary(index, "edition"), std::vector<std::string>{"/acc/edition.epub"});
  EXPECT_EQ(searchLibrary(index, "arger"), std::vector<std::string>{"/acc/\u00C4rger.epub"});
  index.close();
}

TEST_F(LibraryBuilderTest, SearchKeysAreStoredFoldedSoAQueryNeverFoldsABook) {
  fake::reset();
  bookMetadata.clear();
  fake::add("/k/one.epub");
  bookMetadata["/k/one.epub"] = {"L'\u00C9n\u00E9ide", "\u00C9luard", true, "\u00C9dition", 10};
  initial();

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(0, record));
  EXPECT_EQ(std::string(record.fold, record.foldLen), fold("L'\u00C9n\u00E9ide"));
  EXPECT_EQ(std::string(record.authorFold, record.authorFoldLen), fold("\u00C9luard"));
  EXPECT_EQ(std::string(record.seriesFold, record.seriesFoldLen), fold("\u00C9dition"));
  EXPECT_EQ(std::string(record.nameFold, record.nameFoldLen), fold("one"));
  index.close();
}

// --- LIB1-L2: the diff is shown BEFORE anything is promoted -----------------

TEST(LibraryIndexStepperTest, DiffCountsAreFinalBeforeCommitAndNothingIsPromoted) {
  fake::reset();
  fake::add("/a.epub");
  fake::add("/b.epub", "a longer book");
  fake::add("/c.epub", "an even longer book");
  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  const auto before = fake::files[INDEX]->bytes;

  fake::add("/d.epub", "a fourth book of its own size");
  ASSERT_TRUE(Storage.remove("/c.epub"));

  LibraryIndexStepper stepper;
  ASSERT_TRUE(stepper.begin("/", true, /*confirmBeforeCommit=*/true));
  BuildStep step = BuildStep::Progress;
  while (step == BuildStep::Progress || step == BuildStep::Finalizing) step = stepper.step();

  ASSERT_EQ(step, BuildStep::ReadyToCommit);
  const auto progress = stepper.progress();
  EXPECT_EQ(progress.books, 3);
  EXPECT_EQ(progress.added, 1);
  EXPECT_EQ(progress.changed, 0);
  EXPECT_EQ(progress.removed, 1);
  EXPECT_EQ(progress.unchanged, 2);
  // The numbers are on the glass and the shelf has not moved: the journal is a
  // side file until the promote.
  EXPECT_EQ(fake::files[INDEX]->bytes, before);

  stepper.commit();
  step = stepper.step();
  while (step == BuildStep::Progress || step == BuildStep::Finalizing) step = stepper.step();
  EXPECT_EQ(step, BuildStep::Done);
  EXPECT_EQ(stepper.stats().books, 3);
  EXPECT_NE(fake::files[INDEX]->bytes, before);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.part"));
}

TEST(LibraryIndexStepperTest, CancelAtTheDiffKeepsTheServedIndexAndTheJournal) {
  fake::reset();
  tenBooks();
  BuildStats stats;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  const auto before = fake::files[INDEX]->bytes;
  fake::add("/late.epub", "arrived after the last build");

  {
    LibraryIndexStepper stepper;
    ASSERT_TRUE(stepper.begin("/", true, true));
    BuildStep step = BuildStep::Progress;
    while (step == BuildStep::Progress || step == BuildStep::Finalizing) step = stepper.step();
    ASSERT_EQ(step, BuildStep::ReadyToCommit);
    stepper.requestCancel();
    EXPECT_EQ(stepper.step(), BuildStep::Cancelled);
  }

  EXPECT_EQ(fake::files[INDEX]->bytes, before) << "a cancelled commit must not touch the shelf";
  EXPECT_TRUE(Storage.exists("/.crosspoint/library.part"));

  // The journal already holds the finished walk, so the next update promotes
  // without re-reading a single book.
  fake::parses = 0;
  BuildStats again;
  ASSERT_TRUE(buildLibraryIndex("/", again, true));
  EXPECT_EQ(fake::parses, 0u);
  EXPECT_NE(fake::files[INDEX]->bytes, before);
  EXPECT_EQ(again.books, 11);
}
