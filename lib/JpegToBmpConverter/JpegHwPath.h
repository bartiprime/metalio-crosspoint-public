#pragma once

// JPEG1 — the PURE half of the hardware-JPEG decision.
//
// WHY THIS FILE HAS NO ESP/ARDUINO INCLUDE
//   "Which decoder does this file take?" has to be answerable on the host, with
//   real JPEG bytes, before any of it is trusted with a cover. Everything in here
//   is a decision over bytes: the marker scanner (a state machine, fed chunks),
//   the path policy, the luma-diff accumulator. No file I/O, no heap, no driver.
//   test/jpeg_hw_select/ compiles this exact file and runs the table.
//
// WHAT "EQUIVALENT" MEANS HERE
//   The software path (lib/JpegToBmpConverter via JPEGDEC) feeds the rest of the
//   pipeline the JPEG's Y samples, one byte per pixel, full resolution:
//   JPEGDEC's EIGHT_BIT_GRAYSCALE output is JPEGPutMCU8BitGray, which copies the
//   IDCT'd Y block verbatim for 4:2:0/4:2:2/4:4:4 and never touches chroma
//   (jpeg.inl:2809-2900, :5295). So the only thing the hardware may legitimately
//   change is the IDCT itself. Anything else — a different plane, a different
//   stride, chroma leaking in — is a different picture and is treated as a bug:
//   the on-device luma comparison (JpegLumaDiff) rejects it, and the policy below
//   refuses to commit to the hardware in the first place for every input whose
//   layout the driver would change silently.

#include <cstddef>
#include <cstdint>

// The coding process a SOF marker names. Only the two Huffman sequential ones are
// offered to the hardware; everything else is a named software fallback.
enum class JpegCoding : uint8_t {
  Unknown,
  Baseline,              // SOF0  C0
  ExtendedSequential,    // SOF1  C1  (8-bit Huffman sequential; same decode as C0)
  Progressive,           // SOF2  C2
  Lossless,              // SOF3  C3
  DifferentialSequential,  // SOF5/6/7  C5 C6 C7
  ArithmeticSequential,    // SOF9  C9
  ArithmeticProgressive,   // SOF10 CA
  ArithmeticLossless,      // SOF11 CB
};

const char* jpegCodingName(JpegCoding);

// Everything the header says about one file, plus what the caller learned later
// (hasEoi needs the whole stream; the marker scan only needs the header).
struct JpegProbe {
  bool headerOk = false;  // a SOF was parsed and the scan reached SOS/EOI
  int width = 0;
  int height = 0;
  int components = 0;   // 1 = grey, 3 = YCbCr, 4 = CMYK/YCCK
  int sampH0 = 0;       // component 0 (Y) sampling factors
  int sampV0 = 0;
  int sampH1 = 0;  // first chroma component
  int sampV1 = 0;
  JpegCoding coding = JpegCoding::Unknown;
  bool restartInterval = false;  // DRI present — legal, kept for the report
  bool hasEoi = false;           // set by the caller from the whole stream
};

enum class JpegDecodePath : uint8_t { Software, Hardware };

// Every reason a file does NOT take the hardware, and the one reason it does.
// Printed by `jpeg status` / `jpeg compare` and logged per file, so a fallback is
// never silent.
enum class JpegHwReason : uint8_t {
  Eligible = 0,
  NotProbed,            // header unreadable/truncated inside the header
  Progressive,          // SOF2: JPEGDEC decodes progressive at 1/8 anyway
  ArithmeticCoding,     // SOF9/CA/CB: no Huffman tables, driver cannot take it
  CodingNotBaseline,    // lossless / differential / unknown SOF
  ComponentCount,       // 2 or 4 components (CMYK/YCCK, or malformed)
  Sampling444,          // see note: the driver's 4:4:4 path is raw block layout
  SamplingUnsupported,  // eccentric factors (0x12, 0x24, ...) the driver rejects
  NoEoi,                // truncated file: no FFD9 in the stream
  DimensionsOutOfRange, // 0, or beyond the software cap
  StreamTooLarge,       // stream + output buffer beyond the PSRAM budget
  NoPsram,              // budget would fit but the heap does not have it now
  Disabled,             // runtime arm is off
  DecodeFailed,         // the driver was tried and refused/failed (post-hoc)
};
const char* jpegHwReasonName(JpegHwReason);

// Budgets. maxHwBytes is stream + output TOGETHER, because both live in PSRAM at
// the same time and the driver refuses anything outside PSRAM.
//
// WHY THE CAP IS THE CAP: the driver decodes a WHOLE raster — it has no ROI and no
// scale-down — and a colour JPEG costs 3 bytes per pixel (Y,U,V) because RGB is not
// an option (colour conversion clips, see jpegHwLumaBytesPerPixel). 4 MB is what is
// left of an 8 MB PSRAM part once the app is resident, so colour images above
// ~1.4 MP (e.g. a 1200x1800 Calibre cover = 6.5 MB) stay on the software path for
// the honest reason that the raster does not fit. That is a named condition
// (StreamTooLarge), not a silent fallback.
struct JpegHwLimits {
  int maxDimW = 2048;  // same cap the software path enforces (JpegToBmpConverter.cpp)
  int maxDimH = 3072;
  uint32_t maxHwBytes = 4u * 1024u * 1024u;
  uint32_t psramHeadroom = 768u * 1024u;
};

// --- geometry of the hardware output buffer -----------------------------------
// The driver pads the decoded raster to whole MCUs (jpeg_parse_marker.c:193-202),
// so the stride is not the image width.
int jpegHwMcuWidth(const JpegProbe& p);
int jpegHwMcuHeight(const JpegProbe& p);
// Padded output raster width in PIXELS (the row stride).
uint32_t jpegHwStride(const JpegProbe& p);
// Padded output raster height in pixels.
uint32_t jpegHwRasterHeight(const JpegProbe& p);
// Bytes per luma sample in the output: 1 for GRAY (grey JPEG), 3 for the
// interleaved YUV444 raster we ask for on colour JPEGs (luma is byte 0 of each
// pixel). 0 when this file cannot take the hardware at all.
int jpegHwLumaBytesPerPixel(const JpegProbe& p);
// Whole output buffer the driver will ask for, in bytes (0 = unsupported).
uint32_t jpegHwOutputBytes(const JpegProbe& p);

// THE DECISION. Pure: same inputs, same answer, on host and device.
JpegDecodePath jpegSelectPath(const JpegProbe& probe, size_t streamBytes, size_t largestFreePsram,
                              const JpegHwLimits& limits, JpegHwReason* why);

// --- luma comparison ----------------------------------------------------------
// Per-pixel |a-b| statistics of the two decoders' Y planes. The comparison is on
// DECODED LUMINANCE, not on the BMP bytes: the two paths feed the same ditherer
// and the same scaler, so any BMP difference downstream of an identical luma row
// is a function of the luma alone. Comparing the dithered output would compare
// thresholds, not decoders.
struct JpegLumaDiff {
  uint64_t pixels = 0;
  uint64_t differing = 0;  // |d| >= 1
  uint64_t over2 = 0;      // |d| >= 3
  uint64_t over4 = 0;      // |d| >= 5
  uint32_t maxAbs = 0;
  uint64_t sumAbs = 0;

  void add(int a, int b) {
    const int d = (a > b) ? (a - b) : (b - a);
    ++pixels;
    sumAbs += static_cast<uint64_t>(d);
    if (d != 0) ++differing;
    if (d >= 3) ++over2;
    if (d >= 5) ++over4;
    if (static_cast<uint32_t>(d) > maxAbs) maxAbs = static_cast<uint32_t>(d);
  }
  // Compare one row each; `stepA`/`stepB` are the sample strides (1 for GRAY,
  // 3 for an interleaved YUV444 raster whose luma is byte 0).
  void addRow(const uint8_t* a, const uint8_t* b, int n, int stepA = 1, int stepB = 1) {
    for (int i = 0; i < n; i++) add(a[i * stepA], b[i * stepB]);
  }
  double meanAbs() const { return pixels ? static_cast<double>(sumAbs) / static_cast<double>(pixels) : 0.0; }
  double percentDiffering() const { return pixels ? 100.0 * static_cast<double>(differing) / static_cast<double>(pixels) : 0.0; }

  // The acceptance gate for the hardware. NOT "identical": two different IDCT
  // implementations cannot be bit-identical on a photo (that is the documented,
  // deliberate difference). What may never happen is a shifted/misread plane —
  // and that shows up as large deltas on nearly every pixel, not as a couple of
  // levels on edges. So: no pixel may differ by 5 or more, and the mean must stay
  // under 1.5 levels.
  bool matches() const { return pixels > 0 && over4 == 0 && meanAbs() <= 1.5; }
};

// Whole-stream check for FFD9. Entropy-coded data cannot contain FFD9 (the
// encoder stuffs a zero after every FF and RSTn are FFD0..FFD7), so the last
// occurrence in the buffer IS the EOI, and its absence means the file is
// truncated. Scans backwards over the whole buffer.
bool jpegStreamHasEoi(const uint8_t* data, size_t len);

// --- incremental marker scanner ----------------------------------------------
// Fed from a file in whatever chunks the caller has (512 B on device, the whole
// buffer on the host). Stops at SOS, at EOI, or on the first structural error.
// Deliberately small: it only has to answer "what does this header declare?",
// never to decode.
class JpegMarkerScan {
 public:
  void reset();
  // Feed bytes. Returns the number consumed; stops early when done() or failed().
  size_t feed(const uint8_t* data, size_t len);

  bool done() const { return state_ == State::Done; }  // reached SOS or EOI
  bool failed() const { return state_ == State::Failed; }
  const JpegProbe& probe() const { return probe_; }
  int marker() const { return marker_; }

 private:
  enum class State : uint8_t { ExpectMarker, ReadMarker, LenHi, LenLo, Payload, Done, Failed };
  void dispatch();

  State state_ = State::ExpectMarker;
  int marker_ = 0;
  uint32_t remain_ = 0;
  uint16_t len_ = 0;
  uint8_t seg_[40] = {};
  uint8_t segLen_ = 0;
  JpegProbe probe_{};
};
