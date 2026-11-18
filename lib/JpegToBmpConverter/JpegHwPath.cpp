// JPEG1 — pure decision logic. See JpegHwPath.h for why this file has no
// ESP/Arduino includes: test/jpeg_hw_select/ compiles THIS translation unit and
// runs it against a table of headers and against the repo's real JPEGs.

#include "JpegHwPath.h"

const char* jpegCodingName(JpegCoding c) {
  switch (c) {
    case JpegCoding::Baseline: return "baseline(SOF0)";
    case JpegCoding::ExtendedSequential: return "sequential(SOF1)";
    case JpegCoding::Progressive: return "progressive(SOF2)";
    case JpegCoding::Lossless: return "lossless(SOF3)";
    case JpegCoding::DifferentialSequential: return "differential-sequential";
    case JpegCoding::ArithmeticSequential: return "arithmetic-sequential(SOF9)";
    case JpegCoding::ArithmeticProgressive: return "arithmetic-progressive(SOF10)";
    case JpegCoding::ArithmeticLossless: return "arithmetic-lossless(SOF11)";
    default: return "unknown";
  }
}

const char* jpegHwReasonName(JpegHwReason r) {
  switch (r) {
    case JpegHwReason::Eligible: return "eligible";
    case JpegHwReason::NotProbed: return "header unreadable";
    case JpegHwReason::Progressive: return "progressive JPEG";
    case JpegHwReason::ArithmeticCoding: return "arithmetic-coded JPEG";
    case JpegHwReason::CodingNotBaseline: return "not Huffman sequential (lossless/differential)";
    case JpegHwReason::ComponentCount: return "component count (CMYK/YCCK or 2-component)";
    case JpegHwReason::Sampling444: return "4:4:4 sampling (driver would return raw MCU blocks)";
    case JpegHwReason::SamplingUnsupported: return "unsupported sampling factors";
    case JpegHwReason::NoEoi: return "truncated (no EOI)";
    case JpegHwReason::DimensionsOutOfRange: return "dimensions out of range";
    case JpegHwReason::StreamTooLarge: return "stream+output over the PSRAM budget";
    case JpegHwReason::NoPsram: return "not enough PSRAM right now";
    case JpegHwReason::Disabled: return "hardware path switched off at runtime";
    case JpegHwReason::DecodeFailed: return "hardware decoder failed";
    default: return "?";
  }
}

int jpegHwMcuWidth(const JpegProbe& p) {
  int h = p.sampH0;
  if (h < 1) h = 1;
  if (h > 4) h = 4;
  return 8 * h;
}

int jpegHwMcuHeight(const JpegProbe& p) {
  int v = p.sampV0;
  if (v < 1) v = 1;
  if (v > 4) v = 4;
  return 8 * v;
}

uint32_t jpegHwStride(const JpegProbe& p) {
  const uint32_t mcu = static_cast<uint32_t>(jpegHwMcuWidth(p));
  if (p.width <= 0) return 0;
  return ((static_cast<uint32_t>(p.width) + mcu - 1) / mcu) * mcu;
}

uint32_t jpegHwRasterHeight(const JpegProbe& p) {
  const uint32_t mcu = static_cast<uint32_t>(jpegHwMcuHeight(p));
  if (p.height <= 0) return 0;
  return ((static_cast<uint32_t>(p.height) + mcu - 1) / mcu) * mcu;
}

int jpegHwLumaBytesPerPixel(const JpegProbe& p) {
  // Grey JPEG -> the driver's GRAY output (1 B/px, luma only).
  // Colour JPEG -> interleaved YUV444 (V308): Y,U,V per pixel, luma is byte 0.
  // Never RGB: the hardware would clip through RGB and hand back a luma that is
  // not the JPEG's Y (saturated covers differ by tens of levels), which is the
  // one thing this whole exercise must not do.
  if (p.components == 1) return 1;
  if (p.components == 3) return 3;
  return 0;
}

uint32_t jpegHwOutputBytes(const JpegProbe& p) {
  const int bpp = jpegHwLumaBytesPerPixel(p);
  if (bpp == 0) return 0;
  const uint32_t stride = jpegHwStride(p);
  const uint32_t rows = jpegHwRasterHeight(p);
  if (stride == 0 || rows == 0) return 0;
  return stride * rows * static_cast<uint32_t>(bpp);
}

JpegDecodePath jpegSelectPath(const JpegProbe& p, size_t streamBytes, size_t largestFreePsram,
                              const JpegHwLimits& lim, JpegHwReason* why) {
  const auto software = [&](JpegHwReason r) {
    if (why) *why = r;
    return JpegDecodePath::Software;
  };

  if (!p.headerOk) return software(JpegHwReason::NotProbed);
  if (p.coding == JpegCoding::Progressive) return software(JpegHwReason::Progressive);
  if (p.coding == JpegCoding::ArithmeticSequential || p.coding == JpegCoding::ArithmeticProgressive ||
      p.coding == JpegCoding::ArithmeticLossless) {
    return software(JpegHwReason::ArithmeticCoding);
  }
  if (p.coding != JpegCoding::Baseline && p.coding != JpegCoding::ExtendedSequential) {
    return software(JpegHwReason::CodingNotBaseline);
  }

  if (p.components != 1 && p.components != 3) return software(JpegHwReason::ComponentCount);

  if (p.components == 3) {
    const int factors = (p.sampH0 << 4) | p.sampV0;
    // 4:4:4 is REFUSED even though the driver will decode it: with
    // sample_method == output_format the driver sets no_color_conversion and
    // writes the decoder's raw MCU-block layout instead of a raster
    // (jpeg_decode.c:786-789, JPEG_DEC_DIRECT_OUTPUT_HB). Asking for YUV444 from
    // a 4:2:0/4:2:2 stream goes through the CSC and IS a raster, so only those
    // two are offered. 4:4:4 covers are rare; software keeps them exact.
    if (factors == 0x11) return software(JpegHwReason::Sampling444);
    if (factors != 0x22 && factors != 0x21) return software(JpegHwReason::SamplingUnsupported);
  }

  if (p.width <= 0 || p.height <= 0 || p.width > lim.maxDimW || p.height > lim.maxDimH) {
    return software(JpegHwReason::DimensionsOutOfRange);
  }
  if (!p.hasEoi) return software(JpegHwReason::NoEoi);

  const uint32_t outBytes = jpegHwOutputBytes(p);
  if (outBytes == 0) return software(JpegHwReason::SamplingUnsupported);

  const uint64_t need = static_cast<uint64_t>(streamBytes) + static_cast<uint64_t>(outBytes);
  if (need > lim.maxHwBytes) return software(JpegHwReason::StreamTooLarge);
  if (largestFreePsram < need + lim.psramHeadroom) return software(JpegHwReason::NoPsram);

  if (why) *why = JpegHwReason::Eligible;
  return JpegDecodePath::Hardware;
}

bool jpegStreamHasEoi(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 2) return false;
  if (data[len - 2] == 0xFF && data[len - 1] == 0xD9) return true;  // the usual case
  // Backwards: the EOI is the last marker, and FF D9 cannot occur inside entropy
  // data (stuffed FF00 / RSTn FFD0..FFD7), so the first hit from the end is it.
  for (size_t i = len - 2; i > 0;) {
    --i;
    if (data[i] == 0xFF && data[i + 1] == 0xD9) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Marker scanner
// ---------------------------------------------------------------------------
void JpegMarkerScan::reset() {
  state_ = State::ExpectMarker;
  marker_ = 0;
  remain_ = 0;
  len_ = 0;
  segLen_ = 0;
  probe_ = JpegProbe{};
}

void JpegMarkerScan::dispatch() {
  const int m = marker_;
  JpegCoding coding = JpegCoding::Unknown;
  bool isSof = false;
  switch (m) {
    case 0xC0: coding = JpegCoding::Baseline; isSof = true; break;
    case 0xC1: coding = JpegCoding::ExtendedSequential; isSof = true; break;
    case 0xC2: coding = JpegCoding::Progressive; isSof = true; break;
    case 0xC3: coding = JpegCoding::Lossless; isSof = true; break;
    case 0xC5: coding = JpegCoding::DifferentialSequential; isSof = true; break;
    case 0xC6: coding = JpegCoding::Progressive; isSof = true; break;  // differential progressive
    case 0xC7: coding = JpegCoding::Lossless; isSof = true; break;     // differential lossless
    case 0xC9: coding = JpegCoding::ArithmeticSequential; isSof = true; break;
    case 0xCA: coding = JpegCoding::ArithmeticProgressive; isSof = true; break;
    case 0xCB: coding = JpegCoding::ArithmeticLossless; isSof = true; break;
    case 0xCD: coding = JpegCoding::DifferentialSequential; isSof = true; break;
    case 0xCE: coding = JpegCoding::ArithmeticProgressive; isSof = true; break;
    case 0xCF: coding = JpegCoding::ArithmeticLossless; isSof = true; break;
    case 0xDD: probe_.restartInterval = true; break;
    default: break;
  }
  if (!isSof) return;
  if (probe_.headerOk) return;  // a second SOF would be a malformed stream
  if (segLen_ < 6) {
    state_ = State::Failed;
    return;
  }
  probe_.width = (seg_[3] << 8) | seg_[4];
  probe_.height = (seg_[1] << 8) | seg_[2];
  probe_.components = seg_[5];
  probe_.coding = coding;
  if (segLen_ >= 9) {
    probe_.sampH0 = seg_[7] >> 4;
    probe_.sampV0 = seg_[7] & 0x0F;
  }
  if (segLen_ >= 12) {
    probe_.sampH1 = seg_[10] >> 4;
    probe_.sampV1 = seg_[10] & 0x0F;
  }
  probe_.headerOk = true;
}

size_t JpegMarkerScan::feed(const uint8_t* data, size_t len) {
  size_t i = 0;
  while (i < len && state_ != State::Done && state_ != State::Failed) {
    const uint8_t b = data[i++];
    switch (state_) {
      case State::ExpectMarker:
        if (b != 0xFF) {
          state_ = State::Failed;
          break;
        }
        state_ = State::ReadMarker;
        break;

      case State::ReadMarker:
        if (b == 0xFF) break;  // fill byte
        if (b == 0x00) {       // stuffed byte outside entropy data: corrupt
          state_ = State::Failed;
          break;
        }
        marker_ = b;
        if (b == 0xD9 || b == 0xDA) {  // EOI (no image) or SOS (header complete)
          state_ = State::Done;
          break;
        }
        if (b == 0x01 || b == 0xD8 || (b >= 0xD0 && b <= 0xD7)) {  // standalone
          state_ = State::ExpectMarker;
          break;
        }
        state_ = State::LenHi;
        break;

      case State::LenHi:
        len_ = static_cast<uint16_t>(b) << 8;
        state_ = State::LenLo;
        break;

      case State::LenLo: {
        len_ = static_cast<uint16_t>(len_ | b);
        if (len_ < 2) {
          state_ = State::Failed;
          break;
        }
        remain_ = static_cast<uint32_t>(len_) - 2;
        segLen_ = 0;
        if (remain_ == 0) {
          dispatch();
          if (state_ != State::Failed) state_ = State::ExpectMarker;
        } else {
          state_ = State::Payload;
        }
        break;
      }

      case State::Payload:
        if (segLen_ < sizeof(seg_)) seg_[segLen_++] = b;
        if (--remain_ == 0) {
          dispatch();
          if (state_ != State::Failed) state_ = State::ExpectMarker;
        }
        break;

      default:
        break;
    }
  }
  return i;
}
