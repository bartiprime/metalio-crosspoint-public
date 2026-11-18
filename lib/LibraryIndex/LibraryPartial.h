#pragma once

// Append-only build journal (`/.crosspoint/library.part`).
//
// Why a journal exists at all: the index build allocates one staging file for
// the whole library and only learns the last book when the walk returns. A walk
// over a big card is minutes on this panel's SD bus, and a build that cannot be
// stopped is a build the reader has to sit through. So the walk commits ONE
// record at a time into a framed log; stopping between two books leaves a
// durable prefix that the next Update continues from.
//
// What is guaranteed, and by what:
//
//   * A frame is either fully present and CRC-clean, or it is the torn tail and
//     OPEN truncates the file back to the last good frame boundary. CRC over the
//     payload plus a monotonic per-type ordinal is what makes that decision
//     mechanical rather than a guess.
//   * The header carries schema, CLX format, fold version, metadata mode, a
//     rebuild GENERATION and the root path. Any of those disagreeing with the
//     run about to start discards the journal instead of resuming into an index
//     built by different rules.
//   * Nothing here is ever served. The journal is only ever read by the builder;
//     the live index keeps answering until one rename promotes the new file.
//
// Generations, never timestamps: the RTC backup is lost on this board, so files
// this firmware writes carry Jan 1 1980. A staleness rule based on wall-clock
// age would see a 46-year-old journal and rebuild forever. The generation is a
// monotonic counter stored in the index header (ClixHeader::reserved).

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

// Bump when the frame or header layout changes. Independent of CLIX_FORMAT_VERSION:
// the journal can change shape without changing the index it eventually produces.
inline constexpr uint8_t PART_SCHEMA = 1;

inline constexpr uint32_t PART_HEADER_BYTES = 320;
inline constexpr uint16_t PART_FRAME_HEADER_BYTES = 12;

enum class PartFrameType : uint8_t {
  None = 0,
  Folder = 1,  // payload: uint8_t pathLen + path bytes (no trailing '/')
  Record = 2,  // payload: one staged record, PART stride bytes
};

// Everything the journal must agree with before a record is resumed into it.
struct PartIdentity {
  uint32_t generation = 0;      // generation this journal is producing
  uint32_t baseGeneration = 0;  // generation of the index it reconciles against
  uint32_t cardId = 0;          // fnv1a32(rootPath): the mount this was walked from
  uint8_t formatVersion = CLIX_FORMAT_VERSION;
  uint8_t foldVersion = CLIX_FOLD_VERSION;
  bool metadataEnabled = false;
  uint32_t stride = 0;
  std::string rootPath;
};

// Result of OPEN, in the terms the caller actually needs.
enum class PartOpen : uint8_t {
  Resumed,  // existing journal accepted (possibly after truncating a torn tail)
  Fresh,    // no usable journal; an empty one is now open
};

class LibraryPartial {
 public:
  LibraryPartial() = default;
  ~LibraryPartial();
  LibraryPartial(const LibraryPartial&) = delete;
  LibraryPartial& operator=(const LibraryPartial&) = delete;

  // Open (creating when absent). A journal that fails identity or framing
  // checks is discarded and replaced by an empty one, which is why this returns
  // Fresh rather than an error: there is nothing the caller must do about it.
  // `stride` comes from PartIdentity, so the frame size is fixed for the life
  // of the journal.
  PartOpen open(const char* path, const PartIdentity& identity, const uint32_t stride);

  // Sequential replay of the accepted prefix. The sink returns false to stop.
  // Folders arrive first-created-first (walk order), records in staging order,
  // both already validated by OPEN.
  using FrameSink = bool (*)(void* ctx, PartFrameType type, const uint8_t* payload, uint16_t len);
  bool replay(FrameSink sink, void* ctx);

  bool appendFolder(const char* path, uint8_t len);
  bool appendRecord(const void* payload, uint16_t len);

  // The walk reached the end of the card: `total` is now knowable, and the
  // journal may be resumed at exactly `records`.
  bool markScanComplete(uint16_t total);

  uint16_t records() const { return recordCount; }
  uint16_t folders() const { return folderCount; }
  uint16_t total() const { return totalRecords; }
  // The walk that produced this journal reached the end of the card. Nothing is
  // left to scan, so a resume can go straight to sorting and promoting.
  bool scanComplete() const { return walkComplete; }
  uint32_t generation() const { return identity.generation; }
  bool isOpen() const { return opened; }
  bool ioFailed() const { return ioError; }

  void close();
  // Remove the journal: the index it described has been promoted.
  void discard();

  // Read-only header peek for the settings row and the library's defer banner:
  // no identity check, no mutation, no truncation. False when there is no
  // journal on the card.
  static bool peek(const char* path, uint16_t& records, uint16_t& folders, uint16_t& total, uint32_t& generation);

 private:
  bool writeHeader();
  bool appendFrame(PartFrameType type, const uint8_t* payload, uint16_t len, uint16_t ordinal);
  bool truncateAt(uint32_t offset);
  void fail();

  HalFile file;
  std::string path;
  PartIdentity identity;
  uint32_t stride = 0;
  uint32_t dataEnd = 0;  // byte offset just past the last accepted frame
  uint16_t recordCount = 0;
  uint16_t folderCount = 0;
  uint16_t totalRecords = 0;
  bool walkComplete = false;
  bool opened = false;
  bool ioError = false;
};

}  // namespace library
