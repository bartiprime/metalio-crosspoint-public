#include "PpaService.h"

#include "PpaBuffer.h"
#include "PpaSrmPlan.h"  // PPA1: the pure "may the engine serve this?" decision

#include <Logging.h>
#include <driver/ppa.h>
#include <esp_cache.h>
// esp_cache_get_alignment() lives in a private header, but it is under esp_mm's
// public include dir, so REQUIRES esp_mm (which crosspoint_app already has) is
// enough. It is the only correct way to learn the PSRAM cache-line alignment the
// PPA driver will demand of the output buffer.
#include <esp_private/esp_cache_private.h>
#include <esp_heap_caps.h>
#include <esp_flash_encrypt.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <soc/soc_caps.h>

#include <cstring>

namespace ppa {

namespace {

// The test picture: small enough to be cache-friendly and to fit any SoC's
// minimum SRM span, large enough that a wrong stride or a swapped axis cannot
// accidentally match. 32x16 RGB565 = 1 KiB per buffer.
constexpr int kInW = 32;
constexpr int kInH = 16;
constexpr int kBytesPerPixel = 2;

// A pattern that is asymmetric in both axes AND in the low/high byte, so
// mirror_x, mirror_y, a transposed copy and a byte-swapped copy all differ from
// the reference. Every pixel is distinct.
inline uint16_t patternPixel(const int x, const int y) {
  return static_cast<uint16_t>(((y * kInW + x) * 37u + 11u) & 0xFFFFu);
}

struct Case {
  const char* name;
  ppa_srm_rotation_angle_t angle;
  bool mirrorX;
  int dstW;
  int dstH;
};

const Case kCases[] = {
    // Identity and mirror are unambiguous: no interpolation, no rotation
    // convention to argue about. The rotation case is the one that depends on
    // PPA's documented CCW sense, so it is tested separately and reported
    // separately rather than folded into a single pass/fail.
    {"copy", PPA_SRM_ROTATION_ANGLE_0, false, kInW, kInH},
    {"mirror_x", PPA_SRM_ROTATION_ANGLE_0, true, kInW, kInH},
    {"rot90ccw", PPA_SRM_ROTATION_ANGLE_90, false, kInH, kInW},
};
constexpr int kCaseCount = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));

// Byte-identical software reference for one case.
//   copy      : dst(x,y) = src(x,y)
//   mirror_x  : dst(x,y) = src(w-1-x,y)
//   rot90ccw  : dst(X,Y) = src(w-1-Y, X)   (PPA_SRM_ROTATION_ANGLE_90 is CCW,
//                                           components/esp_hal_ppa/include/hal/
//                                           ppa_types.h:28)
void softwareReference(const Case& c, const uint16_t* src, uint16_t* dst) {
  for (int Y = 0; Y < c.dstH; ++Y) {
    for (int X = 0; X < c.dstW; ++X) {
      int sx = X;
      int sy = Y;
      if (c.angle == PPA_SRM_ROTATION_ANGLE_90) {
        sx = kInW - 1 - Y;
        sy = X;
      }
      if (c.mirrorX) sx = kInW - 1 - sx;
      dst[Y * c.dstW + X] = src[sy * kInW + sx];
    }
  }
}

State s_state = State::Disabled;
// The registered client, so a repeated tap (or a run that returned early) can
// never leave a second PPA client behind. Cleared on every exit path.
ppa_client_handle_t s_client = nullptr;

}  // namespace

const char* stateName(const State s) {
  switch (s) {
    case State::Disabled:
      return "disabled";
    case State::Unsupported:
      return "unsupported";
    case State::Ready:
      return "ready";
    case State::Error:
      return "error";
  }
  return "?";
}

State state() {
#if !CP_PPA
  return State::Disabled;
#elif !defined(SOC_PPA_SUPPORTED)
  return State::Unsupported;
#else
  return s_state == State::Disabled ? State::Unsupported : s_state;
#endif
}

SelfTestResult selfTest() {
  SelfTestResult r;
#if !CP_PPA
  // Compiled out entirely: the switch is off, so nothing is linked or run.
  r.state = State::Disabled;
  LOG_INF("TEST", "ppa: disabled (CP_PPA=0)");
  return r;
#elif !defined(SOC_PPA_SUPPORTED)
  // The switch is on but this SoC has no PPA. Report it instead of failing to
  // compile: a ported board must degrade to the software path with a status.
  r.state = State::Unsupported;
  LOG_INF("TEST", "ppa: unsupported on this SoC (CONFIG_SOC_PPA_SUPPORTED=n)");
  return r;
#else
  ppa_client_config_t cfg = {};
  cfg.oper_type = PPA_OPERATION_SRM;
  cfg.max_pending_trans_num = 1;
  cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;
  cfg.flags.allow_pd = 0;  // keep it simple: no register retention to reason about

  // A previous tap that died midway must not leave a client registered: that
  // would both leak a PPA client (the driver has a fixed small pool) and make
  // this "self-test" mutate global driver state.
  if (s_client != nullptr) {
    ppa_unregister_client(s_client);
    s_client = nullptr;
  }

  ppa_client_handle_t client = nullptr;
  const esp_err_t regErr = ppa_register_client(&cfg, &client);
  if (regErr != ESP_OK || client == nullptr) {
    LOG_ERR("TEST", "ppa: register_client failed (%s)", esp_err_to_name(regErr));
    r.state = State::Error;
    s_state = State::Error;
    return r;
  }
  s_client = client;
  r.state = State::Ready;
  s_state = State::Ready;

  // PSRAM, deliberately: whether PPA can reach external RAM is one of the two
  // questions this framework exists to answer on the device
  // (components/esp_driver_ppa/src/ppa_core.c:94 sizes ext_mem_align for
  // MALLOC_CAP_SPIRAM, so PSRAM is a supported class of buffer).
  size_t align = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align) != ESP_OK) align = 64;
  if (align < 64) align = 64;

  constexpr size_t kMaxBytes = kInW * kInH * kBytesPerPixel;
  auto* src = static_cast<uint16_t*>(heap_caps_aligned_alloc(align, kMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto* out = static_cast<uint16_t*>(heap_caps_aligned_alloc(align, kMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto* ref = static_cast<uint16_t*>(heap_caps_aligned_alloc(align, kMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (src == nullptr || out == nullptr || ref == nullptr) {
    LOG_ERR("TEST", "ppa: PSRAM allocation failed (align=%u) - falling back is NOT done here", static_cast<unsigned>(align));
    heap_caps_free(src);
    heap_caps_free(out);
    heap_caps_free(ref);
    ppa_unregister_client(client);
    s_client = nullptr;
    r.state = State::Error;
    s_state = State::Error;
    return r;
  }
  r.inPsram = esp_ptr_external_ram(src);
  r.outPsram = esp_ptr_external_ram(out);

  for (int y = 0; y < kInH; ++y) {
    for (int x = 0; x < kInW; ++x) src[y * kInW + x] = patternPixel(x, y);
  }
  // Make sure the pattern is in PSRAM before the PPA's DMA reads it. The driver
  // msyncs its own windows too (src/ppa_srm.c:276), this is belt and braces for
  // the first access after the write.
  esp_cache_msync(src, kMaxBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

  int64_t ppaTotalUs = 0;
  int64_t softTotalUs = 0;

  for (int i = 0; i < kCaseCount; ++i) {
    const Case& c = kCases[i];
    const size_t outBytes = static_cast<size_t>(c.dstW) * c.dstH * kBytesPerPixel;

    memset(out, 0xA5, kMaxBytes);  // poisoned, so a "wrote nothing" result cannot pass
    memset(ref, 0, kMaxBytes);
    softwareReference(c, src, ref);

    ppa_srm_oper_config_t oper = {};
    oper.in.buffer = src;
    oper.in.pic_w = kInW;
    oper.in.pic_h = kInH;
    oper.in.block_w = kInW;
    oper.in.block_h = kInH;
    oper.in.block_offset_x = 0;
    oper.in.block_offset_y = 0;
    oper.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    oper.out.buffer = out;
    oper.out.buffer_size = static_cast<uint32_t>(outBytes);
    oper.out.pic_w = static_cast<uint32_t>(c.dstW);
    oper.out.pic_h = static_cast<uint32_t>(c.dstH);
    oper.out.block_offset_x = 0;
    oper.out.block_offset_y = 0;
    oper.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    oper.rotation_angle = c.angle;
    oper.scale_x = 1.0f;
    oper.scale_y = 1.0f;
    oper.mirror_x = c.mirrorX;
    oper.mirror_y = false;
    oper.mode = PPA_TRANS_MODE_BLOCKING;

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = ppa_do_scale_rotate_mirror(client, &oper);
    const int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
      LOG_ERR("TEST", "ppa: case %d '%s' failed (%s); inPsram=%d outPsram=%d", i, c.name, esp_err_to_name(err),
              static_cast<int>(r.inPsram), static_cast<int>(r.outPsram));
      r.casesRun++;
      continue;  // counted as a failure: pass stays false
    }
    // Blocking mode returns after the transaction, and the driver invalidated
    // the output window; sync the whole buffer anyway so the comparison cannot
    // read a stale line.
    esp_cache_msync(out, outBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    const int64_t s0 = esp_timer_get_time();
    uint16_t* refCheck = ref;  // the reference was computed above; time a re-run
    softwareReference(c, src, refCheck);
    const int64_t s1 = esp_timer_get_time();

    ppaTotalUs += (t1 - t0);
    softTotalUs += (s1 - s0);

    const uint8_t* a = reinterpret_cast<const uint8_t*>(out);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(ref);
    uint32_t mismatches = 0;
    for (size_t k = 0; k < outBytes; ++k) {
      if (a[k] != b[k]) ++mismatches;
    }
    r.casesRun++;
    r.totalBytes = static_cast<uint32_t>(outBytes);
    if (mismatches == 0) {
      r.casesPassed++;
    } else {
      r.mismatchedBytes = mismatches;
      LOG_ERR("TEST", "ppa: case %d '%s' MISMATCH %lu/%u bytes", i, c.name, static_cast<unsigned long>(mismatches),
              static_cast<unsigned>(outBytes));
    }
  }

  r.pass = (r.casesRun == kCaseCount) && (r.casesPassed == kCaseCount);
  r.ppaUs = ppaTotalUs;
  r.softUs = softTotalUs;

  LOG_INF("TEST", "ppa: %d/%d cases pass=%d inPsram=%d outPsram=%d ppa=%lldus soft=%lldus", r.casesPassed, r.casesRun,
          static_cast<int>(r.pass), static_cast<int>(r.inPsram), static_cast<int>(r.outPsram),
          static_cast<long long>(r.ppaUs), static_cast<long long>(r.softUs));

  esp_cache_msync(src, kMaxBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  heap_caps_free(src);
  heap_caps_free(out);
  heap_caps_free(ref);
  // No PPA client survives this function on any path: the row is a self-test,
  // not an initialisation, and the driver's client pool is small and shared.
  ppa_unregister_client(client);
  s_client = nullptr;
  return r;
#endif
}


// ===========================================================================
// CP_PPA_BENCH — full-frame throughput, measured against a byte-exact
// software reference. See PpaService.h and PpaBuffer.h for the contract.
// ===========================================================================

const char* benchOpName(const BenchOp op) {
  switch (op) {
    case BenchOp::Copy:
      return "copy";
    case BenchOp::Rot90:
      return "rot90";
    case BenchOp::Rot180:
      return "rot180";
    case BenchOp::Rot270:
      return "rot270";
    case BenchOp::MirrorX:
      return "mirror_x";
    case BenchOp::ScaleHalf:
      return "scale0.5";
  }
  return "?";
}

#if CP_PPA_BENCH && CP_PPA

namespace {

// Asymmetric in both axes AND in the pixel value, so mirror/rotate/copy and a
// transposed or byte-swapped result all differ. Same intent as the self-test's
// pattern, generalised to an arbitrary width.
inline uint16_t benchPattern(const int x, const int y, const int w) {
  return static_cast<uint16_t>(((y * w + x) * 37u + 11u) & 0xFFFFu);
}

// Byte-exact software reference for one op, for both pixel sizes.
//   1 B/px (GRAY8): the "pixel" IS the byte.
//   2 B/px (RGB565)
// Rotation sense: PPA_SRM_ROTATION_ANGLE_90 is CCW (hal/ppa_types.h:28).
void benchReference(const BenchOp op, const uint8_t* src, uint8_t* dst, const int w, const int h, const int bpp) {
  const auto* s16 = reinterpret_cast<const uint16_t*>(src);
  auto* d16 = reinterpret_cast<uint16_t*>(dst);
  const int dw = (op == BenchOp::Rot90 || op == BenchOp::Rot270) ? h : w;
  const int dh = (op == BenchOp::Rot90 || op == BenchOp::Rot270) ? w : h;
  for (int Y = 0; Y < dh; ++Y) {
    for (int X = 0; X < dw; ++X) {
      int sx = X;
      int sy = Y;
      switch (op) {
        case BenchOp::Copy:
          break;
        case BenchOp::MirrorX:
          sx = w - 1 - X;
          break;
        case BenchOp::Rot90:  // (x,y) -> (y, w-1-x)
          sx = w - 1 - Y;
          sy = X;
          break;
        case BenchOp::Rot180:
          sx = w - 1 - X;
          sy = h - 1 - Y;
          break;
        case BenchOp::Rot270:  // (x,y) -> (h-1-y, x)
          sx = Y;
          sy = h - 1 - X;
          break;
        case BenchOp::ScaleHalf:  // never compared
          break;
      }
      if (bpp == 1) {
        dst[Y * dw + X] = src[sy * w + sx];
      } else {
        d16[Y * dw + X] = s16[sy * w + sx];
      }
    }
  }
}

}  // namespace

BenchResult benchFullFrame(const BenchOp op, const int wIn, const int hIn, const uint8_t* fb) {
  BenchResult r;
  r.op = op;
  r.opName = benchOpName(op);
  r.fromFb = fb != nullptr;
  r.flashEnc = esp_flash_encryption_enabled();

  // R3 (plan §6): refuse the external-memory path with a stated reason instead of
  // failing inside the driver. esp_driver_ppa/src/ppa_core.c:633 refuses SRM
  // on external memory when flash encryption is on.
  if (r.flashEnc) {
    r.reason =
        "flash encryption is ON: the PPA driver refuses SRM with external memory (ppa_core.c:633). "
        "Not attempting a PSRAM operation.";
    return r;
  }

  const int w = wIn;
  const int h = hIn;
  if (w < 8 || h < 8 || w > 4096 || h > 4096) {
    r.reason = "geometry out of range: need 8..4096 px in both axes";
    return r;
  }
  const int bpp = r.fromFb ? 1 : 2;
  r.w = w;
  r.h = h;
  r.bytesPerPixel = bpp;
  const bool rotate = (op == BenchOp::Rot90 || op == BenchOp::Rot270);
  const bool scale = (op == BenchOp::ScaleHalf);
  r.dw = scale ? w / 2 : (rotate ? h : w);
  r.dh = scale ? h / 2 : (rotate ? w : h);

  const size_t srcBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(bpp);
  const size_t dstBytes = static_cast<size_t>(r.dw) * static_cast<size_t>(r.dh) * static_cast<size_t>(bpp);

  Buffer src;
  if (r.fromFb) {
    // The live framebuffer: adopted, never freed, and its start address is NOT
    // ours to align. If the driver rejects it, that IS the finding (§4.4).
    src.adopt(const_cast<uint8_t*>(fb), srcBytes);
  } else {
    if (!src.alloc(srcBytes, /*preferPsram=*/true)) {
      r.reason = "cannot allocate the aligned source buffer in PSRAM or internal RAM";
      return r;
    }
    if (bpp == 2) {
      auto* s16 = static_cast<uint16_t*>(src.data());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) s16[y * w + x] = benchPattern(x, y, w);
    } else {
      auto* s8 = static_cast<uint8_t*>(src.data());
      for (size_t k = 0; k < srcBytes; ++k) s8[k] = static_cast<uint8_t>((k * 37u + 11u) & 0xFFu);
    }
    src.syncToDevice();
  }

  Buffer dst;
  if (!dst.alloc(dstBytes, /*preferPsram=*/true)) {
    r.reason = "cannot allocate the aligned destination buffer";
    return r;
  }
  Buffer ref;
  if (!ref.alloc(dstBytes, /*preferPsram=*/true)) {
    r.reason = "cannot allocate the aligned reference buffer";
    return r;
  }
  r.inPsram = src.inPsram();
  r.outPsram = dst.inPsram();

  ppa_client_config_t cfg = {};
  cfg.oper_type = PPA_OPERATION_SRM;
  cfg.max_pending_trans_num = 1;
  cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;
  cfg.flags.allow_pd = 0;
  ppa_client_handle_t client = nullptr;
  const esp_err_t regErr = ppa_register_client(&cfg, &client);
  if (regErr != ESP_OK || client == nullptr) {
    r.reason = "ppa_register_client failed (client pool exhausted?)";
    r.driverErr = regErr;
    return r;
  }

  memset(dst.data(), 0xA5, dst.capacity());  // poison: "wrote nothing" cannot pass
  memset(ref.data(), 0x00, ref.capacity());
  if (!scale) benchReference(op, static_cast<const uint8_t*>(src.data()), static_cast<uint8_t*>(ref.data()), w, h, bpp);
  ref.syncToDevice();

  ppa_srm_oper_config_t oper = {};
  oper.in.buffer = src.data();
  oper.in.pic_w = static_cast<uint32_t>(w);
  oper.in.pic_h = static_cast<uint32_t>(h);
  oper.in.block_w = static_cast<uint32_t>(w);  // FULL FRAME: block == picture, offset 0
  oper.in.block_h = static_cast<uint32_t>(h);
  oper.in.block_offset_x = 0;
  oper.in.block_offset_y = 0;
  oper.in.srm_cm = (bpp == 1) ? PPA_SRM_COLOR_MODE_GRAY8 : PPA_SRM_COLOR_MODE_RGB565;
  oper.out.buffer = dst.data();
  oper.out.buffer_size = static_cast<uint32_t>(dstBytes);
  oper.out.pic_w = static_cast<uint32_t>(r.dw);
  oper.out.pic_h = static_cast<uint32_t>(r.dh);
  oper.out.block_offset_x = 0;
  oper.out.block_offset_y = 0;
  oper.out.srm_cm = oper.in.srm_cm;  // same in and out: no format conversion in this bench
  oper.rotation_angle = (op == BenchOp::Rot90)    ? PPA_SRM_ROTATION_ANGLE_90
                        : (op == BenchOp::Rot180) ? PPA_SRM_ROTATION_ANGLE_180
                        : (op == BenchOp::Rot270) ? PPA_SRM_ROTATION_ANGLE_270
                                                  : PPA_SRM_ROTATION_ANGLE_0;
  oper.scale_x = scale ? 0.5f : 1.0f;
  oper.scale_y = scale ? 0.5f : 1.0f;
  oper.mirror_x = (op == BenchOp::MirrorX);
  oper.mirror_y = false;
  oper.mode = PPA_TRANS_MODE_BLOCKING;

  const int64_t t0 = esp_timer_get_time();
  const esp_err_t err = ppa_do_scale_rotate_mirror(client, &oper);
  const int64_t t1 = esp_timer_get_time();
  r.ran = true;
  r.driverErr = err;
  r.ppaUs = t1 - t0;

  if (err != ESP_OK) {
    r.reason = "ppa_do_scale_rotate_mirror returned an error (see driverErr)";
    ppa_unregister_client(client);
    return r;
  }
  dst.syncFromDevice();

  if (!scale) {
    const int64_t s0 = esp_timer_get_time();
    benchReference(op, static_cast<const uint8_t*>(src.data()), static_cast<uint8_t*>(ref.data()), w, h, bpp);
    const int64_t s1 = esp_timer_get_time();
    r.softUs = s1 - s0;
    r.compared = true;
    const uint8_t* a = static_cast<const uint8_t*>(dst.data());
    const uint8_t* b = static_cast<const uint8_t*>(ref.data());
    uint32_t mism = 0;
    for (size_t k = 0; k < dstBytes; ++k) {
      if (a[k] != b[k]) ++mism;
    }
    r.mismatchBytes = mism;
    r.comparedBytes = static_cast<uint32_t>(dstBytes);
    r.exact = (mism == 0);
  }

  if (r.ppaUs > 0) r.ppaMbps = (static_cast<double>(dstBytes) / 1e6) / (static_cast<double>(r.ppaUs) / 1e6);
  if (r.softUs > 0) r.softMbps = (static_cast<double>(dstBytes) / 1e6) / (static_cast<double>(r.softUs) / 1e6);

  ppa_unregister_client(client);
  return r;
}

#endif  // CP_PPA_BENCH && CP_PPA

const char* frameOpName(const FrameOp op) {
  switch (op) {
    case FrameOp::Copy:
      return "copy";
    case FrameOp::Rot180:
      return "rot180";
    case FrameOp::MirrorX:
      return "mirror_x";
  }
  return "?";
}

#if CP_PPA_BENCH && CP_PPA

namespace {

// Nibble swap, built once. The canvas convention (PixFormat.h) is: byte = x / 2,
// and the EVEN x is the LOW nibble. A horizontal mirror therefore maps byte j of
// a row onto byte (rowBytes-1-j) with its two pixels exchanged — that is the
// whole of the CPU path, one table lookup per byte.
inline uint8_t nibSwap(const uint8_t v) { return static_cast<uint8_t>((v >> 4) | (v << 4)); }

void cpuCopy4(const uint8_t* src, uint8_t* dst, const int rowBytes, const int h) {
  for (int y = 0; y < h; ++y) memcpy(dst + (size_t)y * rowBytes, src + (size_t)y * rowBytes, (size_t)rowBytes);
}

void cpuMirror4(const uint8_t* src, uint8_t* dst, const int rowBytes, const int h) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)y * rowBytes;
    uint8_t* d = dst + (size_t)y * rowBytes;
    for (int j = 0; j < rowBytes; ++j) d[j] = nibSwap(s[rowBytes - 1 - j]);
  }
}

void cpuRot1804(const uint8_t* src, uint8_t* dst, const int rowBytes, const int h) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + (size_t)(h - 1 - y) * rowBytes;
    uint8_t* d = dst + (size_t)y * rowBytes;
    for (int j = 0; j < rowBytes; ++j) d[j] = nibSwap(s[rowBytes - 1 - j]);
  }
}

// 4 bpp -> GRAY8. nibble n becomes the BYTE n * 0x11, which is the only choice
// that makes the reverse map exact: (n * 0x11) >> 4 == n for every n in 0..15.
// That is why the sandwich can be pixel-exact and the comparison is a real test
// of the engine rather than of a lossy round trip.
void widen4to8(const uint8_t* src4, uint8_t* dst8, const int rowBytes, const int h) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src4 + (size_t)y * rowBytes;
    uint8_t* d = dst8 + (size_t)y * rowBytes * 2;
    for (int i = 0; i < rowBytes; ++i) {
      const uint8_t lo = static_cast<uint8_t>(s[i] & 0x0F);
      const uint8_t hi = static_cast<uint8_t>(s[i] >> 4);
      d[2 * i] = static_cast<uint8_t>(lo * 0x11);      // x even -> low nibble
      d[2 * i + 1] = static_cast<uint8_t>(hi * 0x11);  // x odd  -> high nibble
    }
  }
}

// GRAY8 -> 4 bpp. The high nibble of each byte; exact for anything widen4to8
// produced, and a stated (documented, not hidden) truncation for anything else.
void narrow8to4(const uint8_t* src8, uint8_t* dst4, const int rowBytes, const int h) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src8 + (size_t)y * rowBytes * 2;
    uint8_t* d = dst4 + (size_t)y * rowBytes;
    for (int i = 0; i < rowBytes; ++i) {
      d[i] = static_cast<uint8_t>(((s[2 * i + 1] >> 4) << 4) | (s[2 * i] >> 4));
    }
  }
}

}  // namespace

bool frameCompare(const FrameOp op, const uint8_t* canvas, const int wIn, const int hIn, FrameCmpResult& out) {
  out.op = op;
  out.w = wIn;
  out.h = hIn;
  out.flashEnc = esp_flash_encryption_enabled();

  SrmRequest req;
  req.format = SrmFormat::Packed4;
  req.op = (op == FrameOp::MirrorX) ? SrmOp::MirrorX : (op == FrameOp::Rot180) ? SrmOp::Rot180 : SrmOp::Copy;
  req.width = static_cast<uint16_t>(wIn);
  req.height = static_cast<uint16_t>(hIn);
  req.dstAddr = reinterpret_cast<uintptr_t>(canvas);
  req.dstBytes = static_cast<size_t>(wIn) * hIn / 2;
  req.outAlign = Buffer::alignment();
  req.flashEncryption = out.flashEnc;
  const SrmPlan plan = planSrm(req);
  out.planned = plan.served;
  out.refusedBecause = refusalName(plan.why);

  // The canvas must be an even-width nibble surface; an odd width has no byte
  // boundary to mirror on, and the comparison would be about a format we do not
  // ship.
  if (canvas == nullptr) {
    out.reason = "no canvas: the framebuffer is not allocated";
    return false;
  }
  if (wIn < 8 || hIn < 8 || wIn > 4096 || hIn > 4096) {
    out.reason = "canvas geometry out of range (8..4096 px per axis)";
    return false;
  }
  if ((wIn & 1) != 0) {
    out.reason = "canvas width is odd: a nibble surface has no byte boundary to mirror on";
    return false;
  }
  if (out.flashEnc) {
    out.reason =
        "flash encryption is ON: the PPA driver refuses SRM with external memory (ppa_core.c:633), "
        "so the PPA half could not run at all";
    return false;
  }

  const int rowBytes = wIn / 2;
  const size_t frameBytes = static_cast<size_t>(rowBytes) * hIn;
  const size_t grayBytes = static_cast<size_t>(wIn) * hIn;
  out.rowBytes = rowBytes;
  out.frameBytes = frameBytes;
  out.scratchBytes = 2 * grayBytes;

  // Two GRAY8 buffers plus the two 4 bpp outputs: ~2.5 MB at 1216x684. Refuse
  // with the number rather than failing halfway.
  ppa::Buffer src8;
  ppa::Buffer dst8;
  if (!src8.alloc(grayBytes, /*preferPsram=*/true) || !dst8.alloc(grayBytes, /*preferPsram=*/true)) {
    out.reason = "cannot allocate the two GRAY8 buffers the sandwich needs";
    return false;
  }
  uint8_t* cpu4 = static_cast<uint8_t*>(heap_caps_aligned_alloc(Buffer::alignment(), frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* ppa4 = static_cast<uint8_t*>(heap_caps_aligned_alloc(Buffer::alignment(), frameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (cpu4 == nullptr || ppa4 == nullptr) {
    heap_caps_free(cpu4);
    heap_caps_free(ppa4);
    out.reason = "cannot allocate the two 4 bpp output buffers";
    return false;
  }
  out.inPsram = src8.inPsram() && dst8.inPsram() && esp_ptr_external_ram(cpu4) && esp_ptr_external_ram(ppa4);

  // ---- path A: the CPU transform -----------------------------------------
  {
    const int64_t t0 = esp_timer_get_time();
    switch (op) {
      case FrameOp::Copy:
        cpuCopy4(canvas, cpu4, rowBytes, hIn);
        break;
      case FrameOp::MirrorX:
        cpuMirror4(canvas, cpu4, rowBytes, hIn);
        break;
      case FrameOp::Rot180:
        cpuRot1804(canvas, cpu4, rowBytes, hIn);
        break;
    }
    out.cpuUs = esp_timer_get_time() - t0;
  }

  // ---- path B: widen -> PPA -> narrow ------------------------------------
  {
    const int64_t t0 = esp_timer_get_time();
    widen4to8(canvas, static_cast<uint8_t*>(src8.data()), rowBytes, hIn);
    src8.syncToDevice();  // the engine's DMA does not see the CPU cache
    out.widenUs = esp_timer_get_time() - t0;
  }

  ppa_client_config_t cfg = {};
  cfg.oper_type = PPA_OPERATION_SRM;
  cfg.max_pending_trans_num = 1;
  cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;
  cfg.flags.allow_pd = 0;
  ppa_client_handle_t client = nullptr;
  const esp_err_t regErr = ppa_register_client(&cfg, &client);
  if (regErr != ESP_OK || client == nullptr) {
    heap_caps_free(cpu4);
    heap_caps_free(ppa4);
    out.reason = "ppa_register_client failed (client pool exhausted?)";
    out.driverErr = regErr;
    return false;
  }

  ppa_srm_oper_config_t oper = {};
  oper.in.buffer = src8.data();
  oper.in.pic_w = static_cast<uint32_t>(wIn);  // GRAY8: 1 B/px, so pic_w is PIXELS
  oper.in.pic_h = static_cast<uint32_t>(hIn);
  oper.in.block_w = static_cast<uint32_t>(wIn);
  oper.in.block_h = static_cast<uint32_t>(hIn);
  oper.in.block_offset_x = 0;
  oper.in.block_offset_y = 0;
  oper.in.srm_cm = PPA_SRM_COLOR_MODE_GRAY8;
  oper.out.buffer = dst8.data();
  oper.out.buffer_size = static_cast<uint32_t>(dst8.capacity());
  oper.out.pic_w = static_cast<uint32_t>(wIn);
  oper.out.pic_h = static_cast<uint32_t>(hIn);
  oper.out.block_offset_x = 0;
  oper.out.block_offset_y = 0;
  oper.out.srm_cm = PPA_SRM_COLOR_MODE_GRAY8;
  oper.rotation_angle = (op == FrameOp::Rot180) ? PPA_SRM_ROTATION_ANGLE_180 : PPA_SRM_ROTATION_ANGLE_0;
  oper.scale_x = 1.0f;
  oper.scale_y = 1.0f;
  oper.mirror_x = (op == FrameOp::MirrorX);
  oper.mirror_y = false;
  oper.mode = PPA_TRANS_MODE_BLOCKING;

  const int64_t p0 = esp_timer_get_time();
  const esp_err_t err = ppa_do_scale_rotate_mirror(client, &oper);
  int64_t p1 = esp_timer_get_time();
  out.driverErr = err;
  out.ran = true;
  if (err != ESP_OK) {
    ppa_unregister_client(client);
    heap_caps_free(cpu4);
    heap_caps_free(ppa4);
    out.reason = "ppa_do_scale_rotate_mirror returned an error (see driverErr)";
    out.ppaUs = p1 - p0;
    return false;
  }
  dst8.syncFromDevice();  // counted in ppaUs: it is the engine's cost, not ours
  p1 = esp_timer_get_time();
  out.ppaUs = p1 - p0;

  {
    const int64_t t0 = esp_timer_get_time();
    narrow8to4(static_cast<const uint8_t*>(dst8.data()), ppa4, rowBytes, hIn);
    out.narrowUs = esp_timer_get_time() - t0;
  }

  ppa_unregister_client(client);

  // ---- the comparison ----------------------------------------------------
  out.comparedBytes = static_cast<uint32_t>(frameBytes);
  uint32_t mism = 0;
  for (size_t k = 0; k < frameBytes; ++k) {
    if (cpu4[k] != ppa4[k]) ++mism;
  }
  out.differingBytes = mism;
  out.identical = (mism == 0);

  heap_caps_free(cpu4);
  heap_caps_free(ppa4);
  return true;
}

#else  // !(CP_PPA_BENCH && CP_PPA)

bool frameCompare(const FrameOp op, const uint8_t* canvas, const int w, const int h, FrameCmpResult& out) {
  (void)canvas;
  (void)w;
  (void)h;
  out.op = op;
  out.reason =
      "not built: this image has CP_PPA=0 or CP_PPA_BENCH=0, so there is no PPA call to measure. "
      "Rebuild with -DCP_PPA=1 -DCP_PPA_BENCH=1 (a bench image; the shipping default is 0 so the image "
      "carries no accelerator symbols).";
  return false;
}

#endif  // CP_PPA_BENCH && CP_PPA

// This chain only runs when the pair above did NOT define benchFullFrame, so a
// bench image with a PPA gets exactly one definition.
#if !CP_PPA_BENCH

BenchResult benchFullFrame(const BenchOp op, const int w, const int h, const uint8_t* fb) {
  (void)w;
  (void)h;
  (void)fb;
  BenchResult r;
  r.op = op;
  r.opName = benchOpName(op);
  r.reason = "not built (CP_PPA_BENCH=0 in src/platform/PlatformFeatures.h)";
  return r;
}

#elif !CP_PPA

BenchResult benchFullFrame(const BenchOp op, const int w, const int h, const uint8_t* fb) {
  (void)w;
  (void)h;
  (void)fb;
  BenchResult r;
  r.op = op;
  r.opName = benchOpName(op);
  r.reason = "this SoC/configuration has no PPA (CONFIG_SOC_PPA_SUPPORTED unset)";
  return r;
}

#endif  // !CP_PPA_BENCH / !CP_PPA

}  // namespace ppa
