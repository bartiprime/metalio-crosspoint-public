#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// PPA1 — "may the PPA SRM engine serve this transform?", answered without a
// driver, a SoC, a heap or a clock.
//
// WHY A SEPARATE HEADER. PpaService.cpp can only ask the question on the
// device, inside CP_PPA_BENCH, and only for cases it already believes are
// legal — a decision that lives next to the code it authorises cannot be tested
// on the host, and this tree has twice shipped an accelerator gate that was
// "obviously" fine. So the decision is a PURE function of a plain description:
// format, geometry, op, buffer addresses, SoC switches. It has no ESP-IDF
// dependency, no allocation, no logging, and it returns a REASON rather than a
// bool, because "the PPA declined" and "there was nothing to decline" are
// different facts and only one of them is a bug.
//
// THE ANSWER FOR THIS PRODUCT, in one line: the SRM engine's colour modes start
// at GRAY8 (1 byte per pixel, hal/ppa_types.h), the renderer's canvas is 4 bpp
// nibbles (lib/GfxRenderer/PixFormat.h), so EVERY render-path buffer this tree
// owns is `FormatNotAddressable` and the CPU path is the only path. That is not
// an oversight being papered over — it is the measured conclusion the r96 bench
// reached and `ppabench ... fb` already refuses on (src/main.cpp). This header
// exists so the conclusion is executable and testable rather than a comment, and
// so that a future 8 bpp surface gets a real answer instead of a copy-paste.
//
// NO PPA SYMBOLS. Nothing here includes <driver/ppa.h>; the header is safe to
// compile into the shipping image, where CP_PPA == 0.
// ---------------------------------------------------------------------------
namespace ppa {

// The pixel formats this renderer can hand a DMA engine. The first three are
// PACKED (several pixels per byte); the rest are byte-addressable.
enum class SrmFormat : uint8_t {
  Packed1,    // 1 bpp, MSB first      — CP_RENDER_4BPP=0 canvas
  Packed2,    // 2 bpp, 4 px/byte      — cover thumbnails (.pxc 2-bit rows)
  Packed4,    // 4 bpp, even x = low   — CP_RENDER_4BPP=1 canvas (SHIPPING)
  Gray8,      // 1 B/px                — PPA_SRM_COLOR_MODE_GRAY8
  Rgb565,     // 2 B/px                — PPA_SRM_COLOR_MODE_RGB565
  Rgb888,     // 3 B/px                — PPA_SRM_COLOR_MODE_RGB888
  Argb8888,   // 4 B/px                — PPA_SRM_COLOR_MODE_ARGB8888
  Yuv420,     // 1.5 B/px, input only  — PPA_SRM_COLOR_MODE_YUV420
  Yuv422,     // 2 B/px                — PPA_SRM_COLOR_MODE_YUV422_*
  Yuv444,     // 3 B/px, input only, via DMA2D before the PPA
};

enum class SrmOp : uint8_t { Copy, Rot90, Rot180, Rot270, MirrorX, MirrorY, Scale };

// Why the engine may not be used. Ordered roughly by how fundamental the
// obstacle is, which is also the order the rules are applied in: a 4 bpp
// canvas is refused for its FORMAT, not for its stride, because no stride
// change would ever make the format work.
enum class Refusal : uint8_t {
  None = 0,
  NoPpa,                 // the SoC or the build has no PPA (CP_PPA==0, !SOC_PPA_SUPPORTED)
  FormatNotAddressable,  // packed sub-byte pixels: the SRM engine floors at GRAY8
  ScaleNotIdentical,     // no interpolation selector -> a scale cannot be proven byte-identical
  ExternalWithFlashEnc,  // SRM refuses a PSRAM buffer when flash encryption is on
  GeometryOutOfRange,    // outside the driver/SoC limits, or a zero axis
  OutputNotAligned,      // out.buffer / out.buffer_size vs the cache line (ppa_core.c:610-643)
};
// Everything the decision needs, and nothing it does not. Defaults describe the
// SHIPPING canvas so a caller that knows nothing still gets the right answer.
struct SrmRequest {
  SrmFormat format = SrmFormat::Packed4;
  SrmOp op = SrmOp::Copy;

  uint16_t width = 0;   // SOURCE, pixels
  uint16_t height = 0;  // SOURCE, pixels
  // Destination geometry. Only Scale differs from the source; for the geometric
  // ops it is derived (rot90/270 swap the axes) and is ignored here.
  uint16_t dstWidth = 0;
  uint16_t dstHeight = 0;

  // Bytes per row, as the caller would hand them to the driver. Not part of the
  // driver's own acceptance check (ppa_core.c does not test stride) but reported
  // back so a caller can see what it asked for.
  uint32_t srcStride = 0;
  uint32_t dstStride = 0;

  uintptr_t dstAddr = 0;  // the OUT buffer's address
  size_t dstBytes = 0;    // the OUT buffer's size, which the driver also checks
  size_t outAlign = 64;   // esp_cache_get_alignment(SPIRAM|DMA); 64 on esp32s31

  bool external = true;          // the buffers live in PSRAM
  bool flashEncryption = false;  // esp_flash_encryption_enabled()
  bool ppaPresent = true;        // CP_PPA && SOC_PPA_SUPPORTED

  // Bytes the caller is willing to spend on a widening buffer. Recorded so the
  // plan can say what the sandwich would cost; it never turns a refusal into a
  // yes, because widening is a full-frame CPU pass and this function only
  // answers "is it legal and identical", not "is it fast".
  size_t scratchBudget = 0;
};

struct SrmPlan {
  bool served = false;   // true => hand this to ppa_do_scale_rotate_mirror()
  Refusal why = Refusal::None;

  // A packed source would have to be widened to GRAY8 (one byte per pixel)
  // before the engine could read it, and narrowed again afterwards.
  // `widenBytes` is the size of ONE such GRAY8 buffer: the canonical cost of
  // putting any packed surface through the PPA, whichever way the plan went.
  bool needsWiden = false;
  size_t widenBytes = 0;

  // What the op would move, so a caller can compare it with the CPU path it
  // would replace without recomputing the geometry itself.
  size_t inBytes = 0;   // bytes the engine would read
  size_t outBytes = 0;  // bytes the engine would write
  uint16_t outWidth = 0;
  uint16_t outHeight = 0;
};

// ===========================================================================
// THE DECISION
//
// Header-only and inline on purpose: `src/**/*.cpp` is globbed at CONFIGURE time
// (idf/components/crosspoint_app/CMakeLists.txt), so a new translation unit
// would only appear in an already-configured build tree after a manual
// reconfigure — a failure mode where the tree silently keeps the old behaviour.
// This is ~80 lines of pure arithmetic, and the house style for that
// (PixFormat.h, DitherUtils.h, StatusFrame.h) is a header.
// ===========================================================================

constexpr const char* refusalName(const Refusal r) {
  switch (r) {
    case Refusal::None:
      return "none";
    case Refusal::NoPpa:
      return "no-ppa";
    case Refusal::FormatNotAddressable:
      return "format-not-addressable";
    case Refusal::ScaleNotIdentical:
      return "scale-not-identical";
    case Refusal::ExternalWithFlashEnc:
      return "srm-refuses-psram-with-flash-encryption";
    case Refusal::GeometryOutOfRange:
      return "geometry-out-of-range";
    case Refusal::OutputNotAligned:
      return "output-not-cache-line-aligned";
  }
  return "?";
}

constexpr const char* formatName(const SrmFormat f) {
  switch (f) {
    case SrmFormat::Packed1:
      return "packed1";
    case SrmFormat::Packed2:
      return "packed2";
    case SrmFormat::Packed4:
      return "packed4";
    case SrmFormat::Gray8:
      return "gray8";
    case SrmFormat::Rgb565:
      return "rgb565";
    case SrmFormat::Rgb888:
      return "rgb888";
    case SrmFormat::Argb8888:
      return "argb8888";
    case SrmFormat::Yuv420:
      return "yuv420";
    case SrmFormat::Yuv422:
      return "yuv422";
    case SrmFormat::Yuv444:
      return "yuv444";
  }
  return "?";
}

constexpr const char* opName(const SrmOp op) {
  switch (op) {
    case SrmOp::Copy:
      return "copy";
    case SrmOp::Rot90:
      return "rot90";
    case SrmOp::Rot180:
      return "rot180";
    case SrmOp::Rot270:
      return "rot270";
    case SrmOp::MirrorX:
      return "mirror_x";
    case SrmOp::MirrorY:
      return "mirror_y";
    case SrmOp::Scale:
      return "scale";
  }
  return "?";
}

constexpr int bitsPerPixel(const SrmFormat f) {
  switch (f) {
    case SrmFormat::Packed1:
      return 1;
    case SrmFormat::Packed2:
      return 2;
    case SrmFormat::Packed4:
      return 4;
    case SrmFormat::Gray8:
      return 8;
    case SrmFormat::Rgb565:
      return 16;
    case SrmFormat::Rgb888:
      return 24;
    case SrmFormat::Argb8888:
      return 32;
    case SrmFormat::Yuv420:
      return 12;
    case SrmFormat::Yuv422:
      return 16;
    case SrmFormat::Yuv444:
      return 24;
  }
  return 0;
}

constexpr bool isPacked(const SrmFormat f) {
  return f == SrmFormat::Packed1 || f == SrmFormat::Packed2 || f == SrmFormat::Packed4;
}

// Driver/SoC geometry window. The lower bound is the SMALLEST span the SRM
// engine will process in one transaction (a burst, not a pixel); the upper bound
// is the 12-bit coordinate space of the block registers, which no panel on this
// board comes close to.
constexpr uint16_t kMinSpan = 8;
constexpr uint16_t kMaxSpan = 4096;

inline SrmPlan planSrm(const SrmRequest& r) {
  SrmPlan p;

  // Output geometry: rot90/270 swap the axes, everything else keeps them.
  const bool quarterTurn = (r.op == SrmOp::Rot90 || r.op == SrmOp::Rot270);
  p.outWidth = (r.op == SrmOp::Scale) ? r.dstWidth : (quarterTurn ? r.height : r.width);
  p.outHeight = (r.op == SrmOp::Scale) ? r.dstHeight : (quarterTurn ? r.width : r.height);

  // The widening cost is a property of the FORMAT, not of the decision, so it is
  // filled even when the request is refused. Any caller that wants to argue with
  // the answer gets the number it would have to pay.
  if (isPacked(r.format)) {
    p.needsWiden = true;
    p.widenBytes = static_cast<size_t>(r.width) * static_cast<size_t>(r.height);  // GRAY8
  }

  // 1. No engine at all. First, because nothing below can matter without it.
  if (!r.ppaPresent) {
    p.why = Refusal::NoPpa;
    return p;
  }

  // 2. Format. The SRM engine's colour modes are GRAY8 and wider
  //    (esp_hal_ppa/include/hal/ppa_types.h: ppa_srm_color_mode_t), so a packed
  //    surface is not addressable at ANY stride, alignment or geometry. This is
  //    the shipping canvas's answer: packed4, 4 pixels per byte.
  if (isPacked(r.format)) {
    p.why = Refusal::FormatNotAddressable;
    return p;
  }

  // 3. Scale. The driver exposes no interpolation selector (ppa_srm_oper_config_t
  //    carries scale_x/scale_y and nothing else), so the engine's resample kernel
  //    is a hardware property the caller cannot pin. A scaled image therefore
  //    cannot be shown byte-identical to any CPU reference, and "visually close"
  //    is not a bar this tree accepts for a picture. Note that YUV444 is also
  //    refused here as an OUTPUT for the same class of reason -- it is input-only
  //    (ppa_types.h) and is converted by DMA2D before the PPA, which is a second
  //    undocumented rounding step.
  if (r.op == SrmOp::Scale) {
    p.why = Refusal::ScaleNotIdentical;
    return p;
  }
  if (r.format == SrmFormat::Yuv444) {
    // Input-only colour mode: reachable only through a DMA2D conversion the
    // caller does not control. Same refusal class, same reason.
    p.why = Refusal::FormatNotAddressable;
    return p;
  }

  // 4. Geometry.
  if (r.width < kMinSpan || r.height < kMinSpan || r.width > kMaxSpan || r.height > kMaxSpan ||
      p.outWidth < kMinSpan || p.outHeight < kMinSpan || p.outWidth > kMaxSpan || p.outHeight > kMaxSpan) {
    p.why = Refusal::GeometryOutOfRange;
    return p;
  }

  // 5. Flash encryption. ppa_check_buffer_alignment() (ppa_core.c:633)
  //    refuses an SRM transaction outright when the buffer is in external memory,
  //    because SRM works in macro blocks whose alignment cannot be forced. It is a
  //    refusal by the driver, not a slow path, so it is detected here.
  if (r.flashEncryption && r.external) {
    p.why = Refusal::ExternalWithFlashEnc;
    return p;
  }

  // 6. Buffer alignment. The driver checks the OUTPUT address AND size against
  //    the cache line for that memory class (ppa_core.c:618) and refuses on
  //    either. Sizes first as a statement of what would be written, so a caller
  //    that computed its buffer from the geometry can see the mismatch.
  const int bpp = bitsPerPixel(r.format);
  const bool planar420 = (r.format == SrmFormat::Yuv420);
  const size_t outBytes = planar420
                              ? static_cast<size_t>(p.outWidth) * p.outHeight * 3 / 2
                              : static_cast<size_t>(p.outWidth) * p.outHeight * bpp / 8;
  p.outBytes = outBytes;
  p.inBytes = planar420 ? static_cast<size_t>(r.width) * r.height * 3 / 2
                        : static_cast<size_t>(r.width) * r.height * bpp / 8;

  const size_t align = r.outAlign == 0 ? 1u : r.outAlign;
  if ((r.dstAddr & (align - 1)) != 0) {
    p.why = Refusal::OutputNotAligned;
    return p;
  }
  if ((r.dstBytes & (align - 1)) != 0) {
    p.why = Refusal::OutputNotAligned;
    return p;
  }
  // A caller that states a buffer smaller than the op would overrun it. That is
  // not the driver's check (it trusts buffer_size), so it is ours.
  if (r.dstBytes != 0 && r.dstBytes < outBytes) {
    p.why = Refusal::OutputNotAligned;
    return p;
  }

  p.served = true;
  p.why = Refusal::None;
  return p;
}

}  // namespace ppa
