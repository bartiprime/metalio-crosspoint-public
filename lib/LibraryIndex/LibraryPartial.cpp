#include "LibraryPartial.h"

#include <Logging.h>

#include <cstring>
#include <memory>
#include <new>

namespace library {
namespace {

constexpr char PART_MAGIC[8] = {'C', 'L', 'X', 'P', 'A', 'R', 'T', '1'};

constexpr uint32_t MAGIC_OFFSET = 0;
constexpr uint32_t SCHEMA_OFFSET = 8;
constexpr uint32_t FORMAT_OFFSET = 9;
constexpr uint32_t FOLD_OFFSET = 10;
constexpr uint32_t METADATA_OFFSET = 11;
constexpr uint32_t GENERATION_OFFSET = 12;
constexpr uint32_t BASE_GENERATION_OFFSET = 16;
constexpr uint32_t CARD_OFFSET = 20;
constexpr uint32_t STRIDE_OFFSET = 24;
constexpr uint32_t RECORDS_OFFSET = 28;
constexpr uint32_t FOLDERS_OFFSET = 32;
constexpr uint32_t TOTAL_OFFSET = 36;
constexpr uint32_t FLAGS_OFFSET = 38;
constexpr uint32_t ROOT_LEN_OFFSET = 39;
constexpr uint32_t ROOT_OFFSET = 40;
constexpr uint32_t ROOT_MAX = 255;
constexpr uint32_t CRC_OFFSET = 300;

constexpr uint8_t FLAG_SCAN_COMPLETE = 1 << 0;

// Nibble-table CRC-32 (IEEE 802.3). Sixteen entries rather than 256: this runs
// once per book on a 1 KB payload, where the table indirection is not the cost.
const uint32_t kCrcNibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ kCrcNibble[crc & 0x0F];
    crc = (crc >> 4) ^ kCrcNibble[crc & 0x0F];
  }
  return crc;
}

uint32_t crc32Of(const void* data, size_t len) {
  return crc32Update(0xFFFFFFFFu, static_cast<const uint8_t*>(data), len) ^ 0xFFFFFFFFu;
}

void putU16(uint8_t* out, uint32_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xFF);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void putU32(uint8_t* out, uint32_t offset, uint32_t value) {
  for (uint32_t i = 0; i < 4; i++) out[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

uint16_t getU16(const uint8_t* in, uint32_t offset) {
  return static_cast<uint16_t>(in[offset] | (static_cast<uint16_t>(in[offset + 1]) << 8));
}

uint32_t getU32(const uint8_t* in, uint32_t offset) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < 4; i++) value |= static_cast<uint32_t>(in[offset + i]) << (8 * i);
  return value;
}

// A-P2-5: a header rewrite is a seek-0, a 320-byte write and a seek back -- two
// extra card transactions plus the cache it evicts, and doing it per book made
// it the single largest source of SD traffic in a walk. The header cannot be
// dropped: it is where the durable counts and the FLAG_SCAN_COMPLETE phase byte
// that skipScan reads live. So it is batched -- once every HEADER_WRITE_EVERY
// frames -- and written unconditionally at the points where the walk can stop
// holding a partial prefix (close(): clean cancel, pause, teardown) and at the
// end of the walk (markScanComplete). An interruption between two of those
// points costs a re-derive of at most HEADER_WRITE_EVERY counts, and OPEN
// re-derives both counts from the frames anyway.
constexpr uint32_t HEADER_WRITE_EVERY = 16;

}  // namespace

LibraryPartial::~LibraryPartial() { close(); }

void LibraryPartial::fail() {
  ioError = true;
}

void LibraryPartial::close() {
  // A-P2-5: this is the unconditional touch point. Clean cancel, pause and the
  // stepper's teardown all arrive here with a partial prefix, and the header is
  // the only place those counts are durable -- appends only write it every
  // HEADER_WRITE_EVERY frames now. Nothing appended (dataEnd still at the first
  // frame) means the header open() wrote is already accurate, and a card that
  // has already failed must not take another write.
  if (opened && !ioError && dataEnd > PART_HEADER_BYTES && !writeHeader()) fail();
  if (opened) file.close();
  opened = false;
}

void LibraryPartial::discard() {
  close();
  if (!path.empty()) Storage.remove(path.c_str());
  path.clear();
  recordCount = 0;
  folderCount = 0;
  totalRecords = 0;
  dataEnd = 0;
}

bool LibraryPartial::writeHeader() {
  uint8_t raw[PART_HEADER_BYTES] = {};
  memcpy(raw + MAGIC_OFFSET, PART_MAGIC, sizeof(PART_MAGIC));
  raw[SCHEMA_OFFSET] = PART_SCHEMA;
  raw[FORMAT_OFFSET] = identity.formatVersion;
  raw[FOLD_OFFSET] = identity.foldVersion;
  raw[METADATA_OFFSET] = identity.metadataEnabled ? 1 : 0;
  putU32(raw, GENERATION_OFFSET, identity.generation);
  putU32(raw, BASE_GENERATION_OFFSET, identity.baseGeneration);
  putU32(raw, CARD_OFFSET, identity.cardId);
  putU32(raw, STRIDE_OFFSET, stride);
  putU32(raw, RECORDS_OFFSET, recordCount);
  putU32(raw, FOLDERS_OFFSET, folderCount);
  putU16(raw, TOTAL_OFFSET, totalRecords);
  raw[FLAGS_OFFSET] = walkComplete ? FLAG_SCAN_COMPLETE : 0;
  const size_t rootLen = identity.rootPath.size() > ROOT_MAX ? ROOT_MAX : identity.rootPath.size();
  raw[ROOT_LEN_OFFSET] = static_cast<uint8_t>(rootLen);
  memcpy(raw + ROOT_OFFSET, identity.rootPath.data(), rootLen);
  putU32(raw, CRC_OFFSET, crc32Of(raw, CRC_OFFSET));

  if (!file.seekSet(0)) return false;
  if (file.write(raw, sizeof(raw)) != sizeof(raw)) return false;
  return file.seekSet(dataEnd);
}

bool LibraryPartial::truncateAt(const uint32_t offset) {
  // Compare with the FILE, not with dataEnd: dataEnd is a cached offset from whichever session
  // last ran, so `offset == dataEnd` never meant "there is nothing past this point". It was the
  // early return that let a torn tail survive while the log above claimed it had been cut.
  if (offset == static_cast<uint32_t>(file.fileSize64())) {
    dataEnd = offset;
    return true;
  }
  if (!file.truncate(offset)) {
    LOG_ERR("LIBPART", "cannot truncate torn tail at %u", static_cast<unsigned>(offset));
    fail();
    return false;
  }
  dataEnd = offset;
  return true;
}

PartOpen LibraryPartial::open(const char* partPath, const PartIdentity& wanted, const uint32_t wantedStride) {
  close();
  ioError = false;
  recordCount = 0;
  folderCount = 0;
  totalRecords = 0;
  walkComplete = false;
  path = partPath;
  identity = wanted;
  stride = wantedStride;

  // O_AT_END positions at the end when the file exists and creates it when it
  // does not, so the append path never truncates a journal it is about to
  // resume from. (openFileForWrite() is O_TRUNC and would erase the prefix.)
  file = Storage.open(path.c_str(), O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("LIBPART", "cannot open %s", path.c_str());
    fail();
    return PartOpen::Fresh;
  }
  opened = true;

  const uint32_t size = static_cast<uint32_t>(file.fileSize64());
  bool usable = size >= PART_HEADER_BYTES;
  uint8_t raw[PART_HEADER_BYTES] = {};
  if (usable) {
    if (!file.seekSet(0) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
      usable = false;
    } else {
      const size_t rootLen = raw[ROOT_LEN_OFFSET] > ROOT_MAX ? ROOT_MAX : raw[ROOT_LEN_OFFSET];
      usable = memcmp(raw + MAGIC_OFFSET, PART_MAGIC, sizeof(PART_MAGIC)) == 0 &&
               getU32(raw, CRC_OFFSET) == crc32Of(raw, CRC_OFFSET) && raw[SCHEMA_OFFSET] == PART_SCHEMA &&
               raw[FORMAT_OFFSET] == wanted.formatVersion && raw[FOLD_OFFSET] == wanted.foldVersion &&
               (raw[METADATA_OFFSET] != 0) == wanted.metadataEnabled &&
               getU32(raw, GENERATION_OFFSET) == wanted.generation &&
               getU32(raw, BASE_GENERATION_OFFSET) == wanted.baseGeneration &&
               getU32(raw, CARD_OFFSET) == wanted.cardId && getU32(raw, STRIDE_OFFSET) == stride &&
               rootLen == wanted.rootPath.size() && memcmp(raw + ROOT_OFFSET, wanted.rootPath.data(), rootLen) == 0;
      if (!usable) {
        LOG_INF("LIBPART", "journal identity/schema mismatch: discarded");
      }
    }
  }

  if (!usable) {
    // Start over rather than resume into an index built by other rules.
    if (!file.truncate(0)) {
      LOG_ERR("LIBPART", "cannot reset %s", path.c_str());
      fail();
      return PartOpen::Fresh;
    }
    // dataEnd is where the first frame lands, not where the header does: leaving
    // it at 0 wrote the first frame over the header it had just written.
    dataEnd = PART_HEADER_BYTES;
    if (!writeHeader()) {
      fail();
      return PartOpen::Fresh;
    }
    return PartOpen::Fresh;
  }

  // Walk the frames. Anything that fails a check is the torn tail: a write that
  // a power cut or a pull interrupted, never a frame the builder considered
  // committed.
  uint32_t pos = PART_HEADER_BYTES;
  uint16_t records = 0;
  uint16_t folders = 0;
  while (pos + PART_FRAME_HEADER_BYTES <= size) {
    uint8_t head[PART_FRAME_HEADER_BYTES] = {};
    if (!file.seekSet(pos) || file.read(head, sizeof(head)) != static_cast<int>(sizeof(head))) break;
    const uint8_t type = head[0];
    const uint16_t len = getU16(head, 2);
    const uint16_t ordinal = getU16(head, 4);
    const uint32_t crc = getU32(head, 8);
    const uint16_t expectedOrdinal = type == static_cast<uint8_t>(PartFrameType::Folder)  ? folders
                                     : type == static_cast<uint8_t>(PartFrameType::Record) ? records
                                                                                           : 0xFFFF;
    if (expectedOrdinal == 0xFFFF || len == 0) break;
    if (type == static_cast<uint8_t>(PartFrameType::Record) && len != stride) break;
    if (pos + PART_FRAME_HEADER_BYTES + len > size) break;
    if (ordinal != expectedOrdinal) break;

    // The CRC runs over the payload only; a 12-byte header is covered by the
    // structural checks above, and a frame whose header is intact but whose
    // payload is short never reaches this point.
    uint32_t running = 0xFFFFFFFFu;
    uint8_t chunk[128];
    uint32_t remaining = len;
    uint16_t consumed = 0;
    while (remaining > 0) {
      const uint32_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
      const int got = file.read(chunk, want);
      if (got <= 0) break;
      running = crc32Update(running, chunk, static_cast<size_t>(got));
      remaining -= static_cast<uint32_t>(got);
      consumed = static_cast<uint16_t>(consumed + got);
    }
    if (consumed != len || (running ^ 0xFFFFFFFFu) != crc) break;

    if (type == static_cast<uint8_t>(PartFrameType::Folder)) {
      folders++;
    } else {
      records++;
    }
    pos += PART_FRAME_HEADER_BYTES + len;
  }

  if (pos != size) {
    LOG_INF("LIBPART", "torn tail at %u of %u: truncated, %u records kept", static_cast<unsigned>(pos),
            static_cast<unsigned>(size), static_cast<unsigned>(records));
  }
  // Truncate BEFORE dataEnd is set: the call below is what decides where the journal ends, and
  // assigning first made it a no-op for exactly the case it exists for.
  if (!truncateAt(pos)) return PartOpen::Fresh;
  recordCount = records;
  folderCount = folders;
  // "partial N of M" is only knowable once the walk that produced this journal
  // reached the end of the card; an interrupted scan leaves M unknown (0).
  const bool headerSaysComplete = (raw[FLAGS_OFFSET] & FLAG_SCAN_COMPLETE) != 0;
  totalRecords = getU16(raw, TOTAL_OFFSET);
  walkComplete = headerSaysComplete && totalRecords == records && pos == size;
  // A-P1-8: A PREFIX THAT IS STILL ON CARD MUST NOT BE REPORTED AS "no journal". These three
  // failures leave a valid prefix behind, and PartOpen::Fresh is the caller's licence to restart
  // ordinals at 0 -- the next append would then write frame 0 over frame 0 and the OPEN after
  // that would discard the preserved work. The third result is therefore ioFailed(): the caller
  // checks ioFailed() before it looks at the mode (LibraryBuilder::begin) and aborts the build
  // with the journal intact. (PartOpen itself cannot grow a member here: LibraryPartial.h is not
  // in this batch's file whitelist.)
  if (!file.seekSet(dataEnd)) {
    LOG_ERR("LIBPART", "cannot seek to %u on resume; keeping the %u-frame prefix", static_cast<unsigned>(dataEnd),
            static_cast<unsigned>(records));
    fail();
    return PartOpen::Fresh;
  }
  // The durable counts are re-derived from the frames, so a header written before
  // a torn frame cannot overstate what survives. Write the corrected header once
  // per OPEN rather than per resume-with-mismatch: this is one 320-byte write,
  // and leaving an overstating header behind would make "partial N of M" lie
  // between the truncation and the first appended record.
  if (!writeHeader()) {
    LOG_ERR("LIBPART", "cannot rewrite the resume header; keeping the %u-frame prefix",
            static_cast<unsigned>(records));
    fail();
    return PartOpen::Fresh;
  }
  return PartOpen::Resumed;
}

bool LibraryPartial::replay(FrameSink sink, void* ctx) {
  if (!opened || sink == nullptr) return false;
  if (!file.seekSet(PART_HEADER_BYTES)) {
    fail();
    return false;
  }
  uint8_t head[PART_FRAME_HEADER_BYTES] = {};
  // A record payload is sizeof(StagedEntry) (~780 bytes today, and it grows with
  // the library's name/author/title fields). Sized from the journal's own stride
  // rather than a constant, on the heap: this must not be a 1 KB stack frame on
  // a 4 KB task.
  std::unique_ptr<uint8_t[]> payload(new (std::nothrow) uint8_t[stride + 1]);
  if (!payload) {
    LOG_ERR("LIBPART", "replay buffer alloc failed (%u bytes)", static_cast<unsigned>(stride));
    fail();
    return false;
  }
  uint32_t pos = PART_HEADER_BYTES;
  while (pos + PART_FRAME_HEADER_BYTES <= dataEnd) {
    if (file.read(head, sizeof(head)) != static_cast<int>(sizeof(head))) {
      fail();
      return false;
    }
    const uint16_t len = getU16(head, 2);
    if (len > stride) {
      // OPEN already accepted this frame, so an oversized length here is a bug,
      // not a torn tail; failing keeps the caller from resuming half a journal.
      LOG_ERR("LIBPART", "frame payload %u exceeds replay buffer", static_cast<unsigned>(len));
      fail();
      return false;
    }
    if (file.read(payload.get(), len) != static_cast<int>(len)) {
      fail();
      return false;
    }
    if (!sink(ctx, static_cast<PartFrameType>(head[0]), payload.get(), len)) return false;
    pos += PART_FRAME_HEADER_BYTES + len;
  }
  if (!file.seekSet(dataEnd)) {
    fail();
    return false;
  }
  return true;
}

bool LibraryPartial::appendFrame(const PartFrameType type, const uint8_t* payload, const uint16_t len,
                                 const uint16_t ordinal) {
  if (!opened || ioError) return false;
  uint8_t head[PART_FRAME_HEADER_BYTES] = {};
  head[0] = static_cast<uint8_t>(type);
  putU16(head, 2, len);
  putU16(head, 4, ordinal);
  putU32(head, 8, crc32Of(payload, len));
  if (!file.seekSet(dataEnd)) {
    fail();
    return false;
  }
  if (file.write(head, sizeof(head)) != sizeof(head) || file.write(payload, len) != len) {
    LOG_ERR("LIBPART", "%s frame write failed at %u", type == PartFrameType::Record ? "record" : "folder",
            static_cast<unsigned>(dataEnd));
    fail();
    return false;
  }
  dataEnd += PART_FRAME_HEADER_BYTES + len;
  // The count moves WITH the frame, before the header is written: a header that
  // trailed its frames by one would make "partial N of M" understate the prefix
  // the very next OPEN accepts.
  if (type == PartFrameType::Folder) {
    folderCount++;
  } else {
    recordCount++;
  }
  // A-P2-5: durability point, but batched. OPEN re-derives both counts from the
  // frames, so a header that trails them by up to HEADER_WRITE_EVERY is only a
  // cost to the next OPEN, not a lie about the prefix.
  const uint32_t frames = static_cast<uint32_t>(recordCount) + static_cast<uint32_t>(folderCount);
  return (frames % HEADER_WRITE_EVERY) != 0 || writeHeader();
}

bool LibraryPartial::appendFolder(const char* folderPath, const uint8_t len) {
  uint8_t payload[1 + 255];
  payload[0] = len;
  memcpy(payload + 1, folderPath, len);
  return appendFrame(PartFrameType::Folder, payload, static_cast<uint16_t>(1 + len), folderCount);
}

bool LibraryPartial::appendRecord(const void* payload, const uint16_t len) {
  return appendFrame(PartFrameType::Record, static_cast<const uint8_t*>(payload), len, recordCount);
}

bool LibraryPartial::markScanComplete(const uint16_t total) {
  if (!opened) return false;
  totalRecords = total;
  walkComplete = true;
  return writeHeader();
}

bool LibraryPartial::peek(const char* peekPath, uint16_t& records, uint16_t& folders, uint16_t& total,
                          uint32_t& generation) {
  records = 0;
  folders = 0;
  total = 0;
  generation = 0;
  HalFile file = Storage.open(peekPath);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  uint8_t raw[PART_HEADER_BYTES] = {};
  const bool read = file.read(raw, sizeof(raw)) == static_cast<int>(sizeof(raw));
  file.close();
  if (!read) return false;
  if (memcmp(raw + MAGIC_OFFSET, PART_MAGIC, sizeof(PART_MAGIC)) != 0) return false;
  if (getU32(raw, CRC_OFFSET) != crc32Of(raw, CRC_OFFSET) || raw[SCHEMA_OFFSET] != PART_SCHEMA) return false;
  records = static_cast<uint16_t>(getU32(raw, RECORDS_OFFSET) & 0xFFFF);
  folders = static_cast<uint16_t>(getU32(raw, FOLDERS_OFFSET) & 0xFFFF);
  total = getU16(raw, TOTAL_OFFSET);
  generation = getU32(raw, GENERATION_OFFSET);
  return true;
}

}  // namespace library
