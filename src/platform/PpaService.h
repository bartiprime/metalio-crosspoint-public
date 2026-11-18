#pragma once

#include <cstddef>
#include <cstdint>

#include "PlatformFeatures.h"

// PPA (Pixel Processing Accelerator) facade. SRM = scale / rotate / mirror; the
// only colour work it does is format conversion, never thresholding or dithering,
// so it cannot replace any part of our 1-bit render path — see
// docs/engineering/LPCORE_AND_PPA_FRAMEWORK.md for where it could realistically
// sit (EPUB image downscale, cover thumbnails, a rotation).
//
// NOT WIRED TO ANYTHING. No renderer path calls this; the only caller is the
// Debug page's self-test.
namespace ppa {

enum class State {
  Disabled,     // CP_PPA == 0: compiled out, no call made
  Unsupported,  // CP_PPA == 1 but CONFIG_SOC_PPA_SUPPORTED is not set on this SoC
  Ready,        // client registered
  Error,
};

// Result of one self-test run. Every sub-test is a byte comparison against a
// software reference — a test that cannot fail would prove nothing.
struct SelfTestResult {
  bool pass = false;
  int casesRun = 0;
  int casesPassed = 0;
  uint32_t mismatchedBytes = 0;  // first failing case: bytes that differed
  uint32_t totalBytes = 0;       // size of one output buffer
  int64_t ppaUs = 0;             // wall time of the PPA operation(s)
  int64_t softUs = 0;            // wall time of the software reference
  bool inPsram = false;          // input buffer landed in PSRAM
  bool outPsram = false;         // output buffer landed in PSRAM
  State state = State::Disabled;
};

State state();
const char* stateName(State s);

// Registers an SRM client, runs the cases, unregisters. Returns the comparison
// result. Safe to call repeatedly.
SelfTestResult selfTest();

// ===========================================================================
// CP_PPA_BENCH — full-frame throughput bench (r96)
// ===========================================================================
enum class BenchOp { Copy, Rot90, Rot180, Rot270, MirrorX, ScaleHalf };

const char* benchOpName(BenchOp op);

struct BenchResult {
  bool ran = false;          // the PPA call was made
  const char* reason = nullptr;  // why it did not run (flash encryption, alloc, driver refuse)
  BenchOp op = BenchOp::Copy;
  const char* opName = "";
  int w = 0, h = 0;          // source geometry in PIXELS
  int dw = 0, dh = 0;        // destination geometry in PIXELS
  int bytesPerPixel = 2;     // 2 = RGB565 (buffer source), 1 = GRAY8 (framebuffer source)
  bool fromFb = false;       // the source was the live 1 bpp framebuffer
  bool inPsram = false, outPsram = false;
  bool flashEnc = false;     // esp_flash_encryption_enabled()
  int64_t ppaUs = 0;         // one blocking ppa_do_scale_rotate_mirror()
  int64_t softUs = 0;        // the same operation in C, single-threaded
  double ppaMbps = 0.0;      // output bytes / time
  double softMbps = 0.0;
  bool compared = false;     // a byte-exact reference applies to this op
  bool exact = false;        // ... and the PPA matched it
  uint32_t comparedBytes = 0;
  uint32_t mismatchBytes = 0;
  int32_t driverErr = 0;     // esp_err_t from the last driver call that failed
};

// One FULL-FRAME SRM operation against a byte-exact software reference.
//
//   op      which transform (ScaleHalf is timed only: the driver exposes no
//           interpolation selector, so a byte-exact reference would be a claim
//           the hardware never made — see ppa_types.h, no mode field)
//   w,h     source size in pixels
//   fb      when non-null: read the source from this framebuffer region
//           (w*h bytes, 1 bpp packed, GRAY8 to the PPA) instead of allocating
//           and filling a pattern. The CALLER must hold RenderLock; this
//           function never takes it.
//
// There is deliberately no sub-rectangle mode: see PpaBuffer.h and plan §4.4.
BenchResult benchFullFrame(BenchOp op, int w, int h, const uint8_t* fb);

// ===========================================================================
// PPA1 — `ppaframe`: the renderer's OWN buffer, both ways, on the device.
//
// THE QUESTION THIS ANSWERS. planSrm() (PpaSrmPlan.h) says the 4 bpp canvas is
// not addressable by the SRM engine, so the CPU path is the only path. The one
// thing arithmetic cannot settle is whether the WIDENING SANDBOX would still be
// worth it: 4 bpp -> GRAY8, one SRM op, GRAY8 -> 4 bpp. That sandwich is
// pixel-exact by construction (nibble n widens to n*0x11 and narrows back to n),
// so the comparison is a real correctness check AND the timing is a real
// answer — and it is measured on the live canvas, not on a synthetic pattern.
//
// The CPU side is the transform the sandwich would replace: a table-driven
// nibble permutation over the same rows. That is the strongest form of the
// question ("even if we needed this transform, would the PPA win?").
// ===========================================================================
enum class FrameOp { Copy, Rot180, MirrorX };

const char* frameOpName(FrameOp op);

struct FrameCmpResult {
  bool ran = false;
  const char* reason = nullptr;  // why it did not run; never a silent zero

  FrameOp op = FrameOp::Copy;
  int w = 0, h = 0;          // canvas geometry, PIXELS
  int rowBytes = 0;          // canvas stride (w / 2)
  size_t frameBytes = 0;     // the canvas, whole
  bool fourBit = true;       // the canvas format actually tested

  // The plan's verdict for this canvas, carried so one command answers both
  // "may we" and "would it pay".
  bool planned = false;
  const char* refusedBecause = "";

  int64_t cpuUs = 0;     // canvas -> canvas, CPU (memcpy for Copy)
  int64_t widenUs = 0;   // 4bpp -> GRAY8
  int64_t ppaUs = 0;     // one blocking ppa_do_scale_rotate_mirror()
  int64_t narrowUs = 0;  // GRAY8 -> 4bpp
  int64_t totalUs() const { return widenUs + ppaUs + narrowUs; }

  uint32_t comparedBytes = 0;
  uint32_t differingBytes = 0;  // PPA sandwich vs the CPU transform
  bool identical = false;

  bool flashEnc = false;
  bool inPsram = false;
  size_t scratchBytes = 0;  // GRAY8 buffers the sandwich needed
  int32_t driverErr = 0;
};

// Runs BOTH paths over `canvas` and fills `out`. The CALLER must hold RenderLock
// (this function never takes it) and `canvas` must be a 4 bpp surface of
// w x h with `rowBytes` = w / 2. Returns false and leaves out.reason set when it
// could not run.
bool frameCompare(FrameOp op, const uint8_t* canvas, int w, int h, FrameCmpResult& out);



}  // namespace ppa
