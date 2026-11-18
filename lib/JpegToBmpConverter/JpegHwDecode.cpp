// JPEG1 — hardware JPEG decode, ESP side.
//
//   esp32s31 has a JPEG codec (CONFIG_SOC_JPEG_DECODE_SUPPORTED) driving 2D-DMA
//   into PSRAM. It takes a WHOLE compressed stream and writes a WHOLE raster, so
//   it is only ever used for files that fit the budget (JpegHwPath.h decides).
//
// Output formats used, and why only these two:
//   * 1-component (grey) JPEG -> JPEG_DECODE_OUT_FORMAT_GRAY, 1 byte/pixel. The
//     codec writes the IDCT'd Y plane; nothing else is involved.
//   * 3-component 4:2:0/4:2:2 JPEG -> JPEG_DECODE_OUT_FORMAT_YUV444 (V308:
//     Y,U,V per pixel), luma = byte 0. The 4:2:0/4:2:2 -> 4:4:4 step goes through
//     the DMA colour-space converter, so the buffer is a plain raster.
//   NOT RGB/RGB565: yuv->rgb clips at 0/255, and a clipped luma is not the JPEG's
//   Y. Saturated covers would shift by tens of levels. Not 4:4:4 either — see
//   jpegSelectPath (the driver would return raw MCU blocks there).

#include "JpegHwDecode.h"

#include <Logging.h>
#include <driver/jpeg_decode.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

constexpr uint32_t kDecodeTimeoutMs = 3000;
constexpr size_t kHeaderBudget = 16 * 1024;

// The EOI is the last thing in a well-formed file; anything after it is padding.
// A tail scan is enough to answer "is this file complete?" without buffering the
// whole stream — the buffered stream re-checks it exactly (see below).
bool fileHasEoi(HalFile& f) {
  const size_t sz = f.size();
  if (sz < 2) return false;

  uint8_t t[2] = {0, 0};
  if (f.seek(sz - 2) && f.read(t, 2) == 2 && t[0] == 0xFF && t[1] == 0xD9) return true;

  const size_t window = sz < 65536 ? sz : 65536;
  if (!f.seek(sz - window)) return false;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[window]);
  if (!buf) return false;
  const int n = f.read(buf.get(), window);
  if (n <= 0) return false;
  return jpegStreamHasEoi(buf.get(), static_cast<size_t>(n));
}

}  // namespace

void JpegHwFrame::release() {
  if (data_) free(data_);
  data_ = nullptr;
  allocBytes_ = 0;
  stride_ = 0;
  width_ = 0;
  height_ = 0;
  bpp_ = 1;
}

const char* jpegHwStatusName(JpegHwStatus s) {
  switch (s) {
    case JpegHwStatus::Ok: return "ok";
    case JpegHwStatus::NoMemory: return "no memory";
    case JpegHwStatus::BadStream: return "bad/truncated stream";
    case JpegHwStatus::DriverReject: return "driver rejected the config";
    case JpegHwStatus::DecodeFailed: return "decode failed";
    case JpegHwStatus::EngineFail: return "engine create failed";
    default: return "?";
  }
}

size_t jpeghwLargestFreePsram() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }

bool jpegProbeFile(HalFile& f, JpegProbe* out) {
  if (out == nullptr) return false;
  *out = JpegProbe{};
  if (!f.seek(0)) return false;

  JpegMarkerScan scan;
  uint8_t buf[512];
  size_t spent = 0;
  while (!scan.done() && !scan.failed() && spent < kHeaderBudget) {
    const int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    scan.feed(buf, static_cast<size_t>(n));
    spent += static_cast<size_t>(n);
  }

  JpegProbe p = scan.probe();
  if (!p.headerOk) {
    f.seek(0);
    return false;
  }
  p.hasEoi = fileHasEoi(f);
  f.seek(0);
  *out = p;
  return true;
}

JpegHwStatus jpegHwDecodeFile(HalFile& f, const JpegProbe& probe, size_t streamBytes, JpegHwFrame* frame,
                              int* driverErr) {
  if (driverErr) *driverErr = 0;
  if (frame == nullptr) return JpegHwStatus::BadStream;
  frame->release();
  if (streamBytes == 0) return JpegHwStatus::BadStream;

  const uint32_t outBytes = jpegHwOutputBytes(probe);
  if (outBytes == 0) return JpegHwStatus::BadStream;
  if (!f.seek(0)) return JpegHwStatus::BadStream;

  // Both buffers must come from jpeg_alloc_decoder_mem: the 2D-DMA needs
  // 16-byte alignment, PSRAM, and an output buffer whose SIZE is cache-line
  // aligned (the allocator rounds it up and reports the real size).
  size_t inAlloc = 0;
  size_t outAlloc = 0;
  jpeg_decode_memory_alloc_cfg_t inCfg = {};
  inCfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  jpeg_decode_memory_alloc_cfg_t outCfg = {};
  outCfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;

  auto* in = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(streamBytes, &inCfg, &inAlloc));
  if (in == nullptr) {
    LOG_WRN("JPG", "JPEG1: no PSRAM for %u-byte stream buffer", (unsigned)streamBytes);
    return JpegHwStatus::NoMemory;
  }
  size_t got = 0;
  while (got < streamBytes) {
    const int n = f.read(in + got, streamBytes - got);
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  if (got != streamBytes) {
    free(in);
    f.seek(0);
    return JpegHwStatus::BadStream;
  }
  // Authoritative truncation check: on the buffered stream, where a missing EOI
  // means the entropy data really does stop early.
  if (!jpegStreamHasEoi(in, streamBytes)) {
    free(in);
    f.seek(0);
    LOG_WRN("JPG", "JPEG1: stream has no EOI (truncated) — software path");
    return JpegHwStatus::BadStream;
  }

  auto* out = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(outBytes, &outCfg, &outAlloc));
  if (out == nullptr) {
    free(in);
    f.seek(0);
    LOG_WRN("JPG", "JPEG1: no PSRAM for %u-byte output raster", (unsigned)outBytes);
    return JpegHwStatus::NoMemory;
  }

  jpeg_decode_engine_cfg_t engCfg = {};
  engCfg.intr_priority = 1;
  engCfg.timeout_ms = static_cast<int>(kDecodeTimeoutMs);
  engCfg.flags.allow_pd = 0;  // one-shot decode: never worth a register backup

  jpeg_decoder_handle_t engine = nullptr;
  const esp_err_t engErr = jpeg_new_decoder_engine(&engCfg, &engine);
  if (engErr != ESP_OK || engine == nullptr) {
    free(in);
    free(out);
    f.seek(0);
    LOG_ERR("JPG", "JPEG1: jpeg_new_decoder_engine failed (%s)", esp_err_to_name(engErr));
    return JpegHwStatus::EngineFail;
  }

  jpeg_decode_cfg_t decCfg = {};
  decCfg.output_format = (jpegHwLumaBytesPerPixel(probe) == 1) ? JPEG_DECODE_OUT_FORMAT_GRAY
                                                              : JPEG_DECODE_OUT_FORMAT_YUV444;
  decCfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  decCfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

  uint32_t outSize = 0;
  const esp_err_t err = jpeg_decoder_process(engine, &decCfg, in, static_cast<uint32_t>(streamBytes), out,
                                             static_cast<uint32_t>(outAlloc), &outSize);
  jpeg_del_decoder_engine(engine);
  free(in);
  f.seek(0);

  if (err != ESP_OK) {
    if (driverErr) *driverErr = static_cast<int>(err);
    free(out);
    LOG_ERR("JPG", "JPEG1: jpeg_decoder_process failed (%s)", esp_err_to_name(err));
    return (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_SUPPORTED) ? JpegHwStatus::DriverReject
                                                                       : JpegHwStatus::DecodeFailed;
  }

  frame->data_ = out;
  frame->allocBytes_ = outAlloc;
  frame->stride_ = jpegHwStride(probe);
  frame->width_ = probe.width;
  frame->height_ = probe.height;
  frame->bpp_ = jpegHwLumaBytesPerPixel(probe);
  LOG_DBG("JPG", "JPEG1: hardware decoded %dx%d (stride %u, %d B/px) into %u of %u bytes", probe.width,
          probe.height, (unsigned)frame->stride_, frame->bpp_, (unsigned)outSize, (unsigned)outAlloc);
  return JpegHwStatus::Ok;
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
namespace jpeghw {
namespace {
Counters s_counters;
}  // namespace

bool armed() { return s_counters.armed; }
void setArmed(bool on) { s_counters.armed = on; }

bool gatePending() { return s_counters.armed && !s_counters.gateDone; }

void resetGate() {
  s_counters.gateDone = false;
  s_counters.gatePassed = false;
  s_counters.gatePixels = 0;
  s_counters.gateMaxAbs = 0;
  s_counters.gateMeanX100 = 0;
  s_counters.armed = true;
}

Counters counters() { return s_counters; }

void noteDecision(JpegDecodePath path, JpegHwReason why) {
  if (path == JpegDecodePath::Hardware) {
    s_counters.hwFiles++;
    return;
  }
  s_counters.swFiles++;
  const int idx = static_cast<int>(why);
  if (idx >= 0 && idx < kReasonCount) s_counters.perReason[idx]++;
}

void noteGateResult(bool passed, const JpegLumaDiff& diff) {
  s_counters.gateDone = true;
  s_counters.gatePassed = passed;
  s_counters.gatePixels = static_cast<uint32_t>(diff.pixels);
  s_counters.gateMaxAbs = diff.maxAbs;
  s_counters.gateMeanX100 = static_cast<uint32_t>(diff.meanAbs() * 100.0 + 0.5);
  if (!passed) {
    // The hardware does not agree with the software on this device, for this
    // file. Do not ship an image we cannot vouch for: software for the rest of
    // the session, and say so loudly enough to be found in `logtail`.
    s_counters.armed = false;
    LOG_ERR("JPG", "JPEG1: hardware luma does NOT match software (max|d|=%u, mean=%.2f, %llu/%llu pixels) — "
                   "hardware path DISABLED for this session",
            diff.maxAbs, diff.meanAbs(), (unsigned long long)diff.over4, (unsigned long long)diff.pixels);
  } else {
    LOG_DBG("JPG", "JPEG1: hardware luma matches software (max|d|=%u, mean=%.2f over %llu pixels) — hardware on",
            diff.maxAbs, diff.meanAbs(), (unsigned long long)diff.pixels);
  }
}

}  // namespace jpeghw
