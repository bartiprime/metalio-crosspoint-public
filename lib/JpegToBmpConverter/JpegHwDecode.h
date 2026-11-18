#pragma once

// JPEG1 — the ESP half: probe a file's header, decode it with the SoC's JPEG
// codec into a PSRAM raster, and keep the RAM-only runtime state the debug shell
// reports (`jpeg`). The decision itself lives in JpegHwPath.h, which has no ESP
// includes and is unit-tested on the host.
//
// THE ONE INVARIANT
//   This module only ever hands back the JPEG's LUMA (Y) samples, in the same
//   grid and the same unit as JPEGDEC's EIGHT_BIT_GRAYSCALE output — the thing
//   JpegToBmpConverter already consumes. It never returns RGB: colour conversion
//   clips saturated pixels and a clipped luma is not the JPEG's Y.

#include <HalStorage.h>

#include <cstdint>

#include "JpegHwPath.h"

// A decoded luma plane living in PSRAM (allocated by the driver's own allocator,
// which is what makes it 2D-DMA legal: 16-byte aligned, PSRAM, unencrypted).
class JpegHwFrame {
 public:
  JpegHwFrame() = default;
  ~JpegHwFrame() { release(); }
  JpegHwFrame(const JpegHwFrame&) = delete;
  JpegHwFrame& operator=(const JpegHwFrame&) = delete;

  bool valid() const { return data_ != nullptr; }
  int width() const { return width_; }
  int height() const { return height_; }
  uint32_t stride() const { return stride_; }  // pixels, NOT bytes
  int bpp() const { return bpp_; }
  // Row of the decoded raster; luma is byte 0 of each pixel when bpp()==3.
  const uint8_t* row(int y) const { return data_ + static_cast<size_t>(y) * stride_ * static_cast<size_t>(bpp_); }

  void release();

  // Filled by jpegHwDecodeFile(); public so the decode function can hand over
  // ownership without a friend declaration.
  uint8_t* data_ = nullptr;
  size_t allocBytes_ = 0;
  uint32_t stride_ = 0;
  int width_ = 0;
  int height_ = 0;
  int bpp_ = 1;
};

enum class JpegHwStatus : uint8_t { Ok, NoMemory, BadStream, DriverReject, DecodeFailed, EngineFail };
const char* jpegHwStatusName(JpegHwStatus);

// Largest contiguous free PSRAM block right now (0 when PSRAM is off).
size_t jpeghwLargestFreePsram();

// Read the header (<= 16 KB) and the trailing EOI marker. Leaves the file at 0.
bool jpegProbeFile(HalFile& f, JpegProbe* out);

// Decode the whole file. On failure the frame is untouched and the file is left
// at 0, so the caller can hand the same HalFile to the software decoder.
JpegHwStatus jpegHwDecodeFile(HalFile& f, const JpegProbe& probe, size_t streamBytes, JpegHwFrame* frame,
                              int* driverErr);

// ---------------------------------------------------------------------------
// Runtime state (RAM only — nothing persisted, nothing survives a reboot).
// ---------------------------------------------------------------------------
namespace jpeghw {

constexpr int kReasonCount = 14;  // JpegHwReason values

struct Counters {
  bool armed = true;
  bool gateDone = false;    // has the first-use comparison run this boot?
  bool gatePassed = false;  // did the hardware agree with the software?
  uint32_t hwFiles = 0;
  uint32_t swFiles = 0;
  uint32_t perReason[kReasonCount] = {};
  uint32_t gatePixels = 0;
  uint32_t gateMaxAbs = 0;
  uint32_t gateMeanX100 = 0;
};

bool armed();
void setArmed(bool on);
// True until the first eligible image of this boot has been graded against the
// software decoder. The gate exists so a layout mistake in the driver cannot ship
// a wrong cover: on a mismatch the hardware is switched off for the session.
bool gatePending();
void resetGate();  // `jpeg gate` — re-arm the comparison after a flash
Counters counters();
void noteDecision(JpegDecodePath path, JpegHwReason why);
void noteGateResult(bool passed, const JpegLumaDiff& diff);

}  // namespace jpeghw
