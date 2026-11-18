// r163 A13: the live page record.
//
// Two claims are proven here, both on the host and both as counts rather than timings:
//
//  1. What a page load used to cost. Page::deserialize() reads every scalar of the record with
//     its own HalFile::read(), and on the card each of those is a separate SD round trip. The
//     test serialises a real page (the same bytes Section writes) and counts those calls.
//  2. What a repaint now costs. PageRecordCache copies the record in with one read, then serves
//     it from RAM: a second load of the same page issues no file reads at all and produces the
//     same page, element for element.
#include <Epub/Page.h>
#include <Epub/PageRecordCache.h>
#include <Epub/PageRecordRange.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <MemReader.h>
#include <Memory.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kRecordFile = "page_record_cache_test.bin";

// The records these tests write start at offset 0: the section header is Section's business and
// pageRecordRange() only needs to know where records may not live.
constexpr uint32_t kRecordingFileHeaderSize = 0;

// The stub renderer measures every glyph at 8 px, so a 1000 px line holds this sentence without
// wrapping and each PageLine carries one TextBlock.
const std::vector<const char*> kWords = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"};

std::unique_ptr<Page> makeTextPage(const int lines) {
  auto page = makeUniqueNoThrow<Page>();
  if (!page) return nullptr;
  GfxRenderer renderer;
  for (int i = 0; i < lines; ++i) {
    BlockStyle style;
    ParsedText text(/*hyphenation=*/false, /*focusReading=*/false, style, 0);
    for (const char* word : kWords) text.addWord(word, EpdFontFamily::REGULAR);
    std::unique_ptr<TextBlock> block;
    text.layoutAndExtractLines(renderer, 0, 1000, [&block](std::unique_ptr<TextBlock> line, auto) {
      if (!block) block = std::move(line);
    });
    if (!block) return nullptr;
    auto element = makeUniqueNoThrow<PageLine>(std::move(block), 0, static_cast<int16_t>(i * 20));
    if (!element) return nullptr;
    page->elements.push_back(std::move(element));
  }
  return page;
}

// The text of every line, so "same page" can be asserted as content rather than as pointer or
// element-count equality.
std::vector<std::string> lineText(const Page& page) {
  std::vector<std::string> lines;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    std::string text;
    for (uint16_t i = 0; i < line.getBlock()->wordCount(); ++i) {
      if (!text.empty()) text += ' ';
      text += line.getBlock()->wordText(i);
    }
    lines.push_back(text);
  }
  return lines;
}

// Write `pages` back to back into one file, exactly as Section::onPageComplete() does, and report
// where each record starts.
std::vector<uint32_t> writeRecords(const std::vector<Page*>& pages) {
  HalFile file;
  EXPECT_TRUE(file.open(kRecordFile, "wb"));
  std::vector<uint32_t> offsets;
  for (Page* page : pages) {
    offsets.push_back(static_cast<uint32_t>(file.position()));
    EXPECT_TRUE(page->serialize(file));
  }
  EXPECT_TRUE(file.close());
  return offsets;
}

std::vector<uint8_t> readWholeFile() {
  std::vector<uint8_t> bytes;
  HalFile file;
  if (!file.open(kRecordFile, "rb")) return bytes;
  bytes.resize(file.size());
  if (!bytes.empty()) {
    EXPECT_EQ(file.read(bytes.data(), bytes.size()), bytes.size());
  }
  return bytes;
}

class PageRecordCacheTest : public ::testing::Test {
 protected:
  void TearDown() override {
    std::remove(kRecordFile);
    HalFileTest::failSeekOnCall = 0;
    HalFileTest::seekCalls = 0;
  }
};

// The cold path: what a page load costs on the card today. Every scalar of every element is one
// HalFile::read(); the trace that opened this work measured ~8 reads per text line, ~340 for a
// 40-line page.
TEST_F(PageRecordCacheTest, ColdDeserializeCostIsOneReadPerField) {
  constexpr int kLines = 40;
  auto page = makeTextPage(kLines);
  ASSERT_NE(page, nullptr);
  ASSERT_EQ(page->elements.size(), static_cast<size_t>(kLines));
  const auto offsets = writeRecords({page.get()});
  ASSERT_EQ(offsets.size(), 1u);

  HalFile file;
  ASSERT_TRUE(file.open(kRecordFile, "rb"));
  HalFileTest::readCalls = 0;
  auto loaded = Page::deserialize(file);
  const size_t coldReads = HalFileTest::readCalls;

  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(lineText(*loaded), lineText(*page));
  // 40 lines, one read per field plus one per line for the text arena: the count is bounded well
  // below one read per byte and well above one read per line, which is exactly the per-scalar
  // behaviour the cache removes.
  EXPECT_GT(coldReads, static_cast<size_t>(kLines) * 8) << "per-field reads: " << coldReads;
  EXPECT_LT(coldReads, static_cast<size_t>(kLines) * 64) << "per-field reads: " << coldReads;
  EXPECT_GT(coldReads, 300u) << "a 40-line page should cost the ~340 reads the trace counted, got " << coldReads;
  std::cout << "[ A13 ] cold page-record deserialize: " << coldReads << " HalFile::read calls for " << kLines
            << " text lines\n";
}

// The same record off RAM: no file reads, same page.
TEST_F(PageRecordCacheTest, RamDeserializeIsIdenticalAndTouchesNothing) {
  auto page = makeTextPage(40);
  ASSERT_NE(page, nullptr);
  writeRecords({page.get()});

  const auto bytes = readWholeFile();
  ASSERT_FALSE(bytes.empty());
  MemReader reader(bytes.data(), bytes.size());
  HalFileTest::readCalls = 0;
  auto loaded = Page::deserialize(reader);
  EXPECT_EQ(HalFileTest::readCalls, 0u);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(lineText(*loaded), lineText(*page));
}

// A truncated RAM buffer fails the record instead of reading past the end of the buffer.
TEST_F(PageRecordCacheTest, TruncatedRamRecordIsRejected) {
  auto page = makeTextPage(4);
  ASSERT_NE(page, nullptr);
  writeRecords({page.get()});
  auto bytes = readWholeFile();
  ASSERT_GT(bytes.size(), 16u);
  bytes.resize(bytes.size() / 2);
  MemReader reader(bytes.data(), bytes.size());
  EXPECT_EQ(Page::deserialize(reader), nullptr);
}

// The range rule: a record ends where the next page begins; the last one ends at the page LUT.
TEST(PageRecordRange, BoundsFollowTheLutAndThePageBlockEnd) {
  constexpr uint32_t kHeader = 40;
  constexpr uint32_t kMax = 16 * 1024;

  const auto middle = pageRecordRange(1000, 1200, /*lastPage=*/false, /*pageBlockEnd=*/9000, kHeader, kMax);
  EXPECT_TRUE(middle.valid);
  EXPECT_EQ(middle.start, 1000u);
  EXPECT_EQ(middle.end, 1200u);

  const auto last = pageRecordRange(8800, 0, /*lastPage=*/true, /*pageBlockEnd=*/9000, kHeader, kMax);
  EXPECT_TRUE(last.valid);
  EXPECT_EQ(last.start, 8800u);
  EXPECT_EQ(last.end, 9000u);

  // Nonsense ranges are refused rather than cached: empty, backwards, inside the header, and
  // larger than the one-record bound.
  EXPECT_FALSE(pageRecordRange(1200, 1200, false, 9000, kHeader, kMax).valid);
  EXPECT_FALSE(pageRecordRange(1200, 1000, false, 9000, kHeader, kMax).valid);
  EXPECT_FALSE(pageRecordRange(10, 1200, false, 9000, kHeader, kMax).valid);
  EXPECT_FALSE(pageRecordRange(1000, 1000 + kMax + 1, false, 9000, kHeader, kMax).valid);
  EXPECT_FALSE(pageRecordRange(1000, 1200, true, 0, kHeader, kMax).valid);
}

// The cache end to end: one bulk read for the record, then repaints of the same page are free.
TEST_F(PageRecordCacheTest, RepaintOfTheSamePageIssuesNoReads) {
  auto page0 = makeTextPage(10);
  auto page1 = makeTextPage(20);
  ASSERT_NE(page0, nullptr);
  ASSERT_NE(page1, nullptr);
  const auto offsets = writeRecords({page0.get(), page1.get()});
  ASSERT_EQ(offsets.size(), 2u);

  HalFile file;
  ASSERT_TRUE(file.open(kRecordFile, "rb"));
  const uint32_t blockEnd = static_cast<uint32_t>(file.size());

  PageRecordCache cache;
  page0->visibleTextOffset = 4242;
  const PageRecordRange first =
      pageRecordRange(offsets[0], offsets[1], /*lastPage=*/false, blockEnd, kRecordingFileHeaderSize, 16 * 1024);
  ASSERT_TRUE(first.valid);

  HalFileTest::readCalls = 0;
  auto loaded = cache.load(0, /*fromBuild=*/false, file, first, 0, 4242);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(lineText(*loaded), lineText(*page0));
  EXPECT_EQ(loaded->visibleTextOffset, 4242u);
  // One read for the whole record -- not one per field.
  EXPECT_EQ(HalFileTest::readCalls, 1u);
  EXPECT_EQ(cache.stats().refreshes, 1u);
  EXPECT_EQ(cache.stats().hits, 0u);

  // The repaint: same page, no card access at all.
  HalFileTest::readCalls = 0;
  auto again = cache.load(0, /*fromBuild=*/false, file, first, 0, 4242);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(HalFileTest::readCalls, 0u);
  EXPECT_EQ(lineText(*again), lineText(*page0));
  EXPECT_EQ(cache.stats().hits, 1u);
  EXPECT_EQ(cache.stats().sdReads, 1u);

  // A page turn: the new page is read once, and the old record is gone (one page, not a chapter).
  const PageRecordRange second =
      pageRecordRange(offsets[1], 0, /*lastPage=*/true, blockEnd, kRecordingFileHeaderSize, 16 * 1024);
  ASSERT_TRUE(second.valid);
  HalFileTest::readCalls = 0;
  auto next = cache.load(1, /*fromBuild=*/false, file, second, 0, 0);
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(HalFileTest::readCalls, 1u);
  EXPECT_EQ(lineText(*next), lineText(*page1));
  EXPECT_EQ(cache.stats().refreshes, 2u);

  // Invalidation (a build starting, committing or being abandoned, or the file being reloaded)
  // drops the copy: the next load reads again.
  cache.invalidate();
  HalFileTest::readCalls = 0;
  auto afterInvalidate = cache.load(1, /*fromBuild=*/false, file, second, 0, 0);
  ASSERT_NE(afterInvalidate, nullptr);
  EXPECT_EQ(HalFileTest::readCalls, 1u);
  EXPECT_EQ(cache.stats().refreshes, 3u);
}

// The same page index served by the build's tmp .bin and by the committed file are different
// bytes: the cache must not answer one with the other.
TEST_F(PageRecordCacheTest, BuildAndDiskSourcesAreNotInterchangeable) {
  auto page = makeTextPage(5);
  ASSERT_NE(page, nullptr);
  const auto offsets = writeRecords({page.get()});
  ASSERT_EQ(offsets.size(), 1u);

  HalFile file;
  ASSERT_TRUE(file.open(kRecordFile, "rb"));
  const uint32_t blockEnd = static_cast<uint32_t>(file.size());
  const PageRecordRange range = pageRecordRange(offsets[0], 0, true, blockEnd, kRecordingFileHeaderSize, 16 * 1024);
  ASSERT_TRUE(range.valid);

  PageRecordCache cache;
  ASSERT_NE(cache.load(0, /*fromBuild=*/true, file, range, 0, 0), nullptr);
  EXPECT_EQ(cache.stats().refreshes, 1u);
  HalFileTest::readCalls = 0;
  ASSERT_NE(cache.load(0, /*fromBuild=*/false, file, range, 0, 0), nullptr);
  EXPECT_EQ(HalFileTest::readCalls, 1u) << "a disk load must not be answered from the build's copy";
  EXPECT_EQ(cache.stats().refreshes, 2u);
}

// One record, fixed cost: a record past the cache's bound is refused, so the caller falls back to
// the streaming read instead of the cache growing with the book.
TEST_F(PageRecordCacheTest, OversizedRecordIsRefused) {
  auto page = makeTextPage(2);
  ASSERT_NE(page, nullptr);
  writeRecords({page.get()});
  HalFile file;
  ASSERT_TRUE(file.open(kRecordFile, "rb"));
  PageRecordCache cache;
  PageRecordRange huge;
  huge.start = 16;
  huge.end = 16 + 16 * 1024 + 1;
  huge.valid = true;
  EXPECT_EQ(cache.load(0, false, file, huge, 0, 0), nullptr);
  EXPECT_EQ(cache.stats().refreshes, 0u);
  EXPECT_EQ(cache.stats().sdReads, 0u);
}

// A failed seek is not a cache miss. Unchecked, the read that follows it still returns a
// full-length window -- of the bytes at whatever offset the handle was left at -- and the record
// deserialises into a page that LOOKS valid. Both seeks are load-bearing: the first places the
// read, the second puts the caller's cursor (a build's append position) back.
TEST_F(PageRecordCacheTest, FailedSeekReturnsNoCacheInsteadOfWrongBytes) {
  auto page0 = makeTextPage(8);
  auto page1 = makeTextPage(12);
  ASSERT_NE(page0, nullptr);
  ASSERT_NE(page1, nullptr);
  const auto offsets = writeRecords({page0.get(), page1.get()});
  ASSERT_EQ(offsets.size(), 2u);

  HalFile file;
  ASSERT_TRUE(file.open(kRecordFile, "rb"));
  const uint32_t blockEnd = static_cast<uint32_t>(file.size());
  PageRecordCache cache;
  const PageRecordRange first =
      pageRecordRange(offsets[0], offsets[1], /*lastPage=*/false, blockEnd, kRecordingFileHeaderSize, 16 * 1024);
  ASSERT_TRUE(first.valid);

  // 1: the record read's own seek fails -> no cache, no read, and the caller streams instead.
  HalFileTest::seekCalls = 0;
  HalFileTest::failSeekOnCall = 1;
  EXPECT_EQ(cache.load(0, /*fromBuild=*/false, file, first, 0, 1234), nullptr)
      << "a failed seek must not be answered with a page read from the wrong offset";
  EXPECT_EQ(cache.stats().refreshes, 0u);
  EXPECT_EQ(cache.stats().sdReads, 0u) << "nothing was read, so nothing was charged";

  // 2: the restore seek fails -> the bytes may be right but the caller's cursor is not.
  HalFileTest::seekCalls = 0;
  HalFileTest::failSeekOnCall = 2;
  EXPECT_EQ(cache.load(0, /*fromBuild=*/false, file, first, 0, 1234), nullptr);
  EXPECT_EQ(cache.stats().refreshes, 0u) << "a half-done read is not cached";

  // 3: the same load with both seeks working succeeds, so the two failures above are the seeks.
  HalFileTest::failSeekOnCall = 0;
  auto ok = cache.load(0, /*fromBuild=*/false, file, first, 0, 1234);
  ASSERT_NE(ok, nullptr);
  EXPECT_EQ(lineText(*ok), lineText(*page0));
  EXPECT_EQ(ok->visibleTextOffset, 1234u);
  EXPECT_EQ(cache.stats().refreshes, 1u);
}

// The page-start offset is not in the record body, so it is stamped on EVERY load from the value
// the caller passed -- a hit included. Remembering the offset the bytes were copied with would
// answer a repaint after a re-pagination with the old page's offset.
TEST_F(PageRecordCacheTest, AHitStampsTheCallersOffset) {
  auto page = makeTextPage(6);
  ASSERT_NE(page, nullptr);
  const auto offsets = writeRecords({page.get()});
  ASSERT_EQ(offsets.size(), 1u);

  HalFile file;
  ASSERT_TRUE(file.open(kRecordFile, "rb"));
  const uint32_t blockEnd = static_cast<uint32_t>(file.size());
  PageRecordCache cache;
  const PageRecordRange range =
      pageRecordRange(offsets[0], 0, /*lastPage=*/true, blockEnd, kRecordingFileHeaderSize, 16 * 1024);
  ASSERT_TRUE(range.valid);

  auto first = cache.load(0, /*fromBuild=*/false, file, range, 0, 111);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->visibleTextOffset, 111u);
  auto second = cache.load(0, /*fromBuild=*/false, file, range, 0, 222);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(cache.stats().hits, 1u) << "same page, same source: answered from RAM";
  EXPECT_EQ(second->visibleTextOffset, 222u) << "the caller's LUT, not the cached one";
}

}  // namespace
