// PPA1 — the SRM selection decision, on the host.
//
// WHAT THIS IS FOR. The PPA cannot be run here, so "the hardware path is
// identical" cannot be tested. The DECISION can be: it is a pure function of a
// buffer description, and every case below is a real buffer this product either
// owns or could own. A test that only asserted "served == false" would pass for
// the wrong reason the moment the rule order changed, so each case names the
// REFUSAL it must produce, and the summary counts them per code.
//
// The table is the claim in executable form:
//   * every render-path surface (packed1/2/4) is FormatNotAddressable, and the
//     plan still reports what a widening buffer would cost;
//   * a byte-addressable full frame is SERVED, so the function is not a
//     one-way switch dressed up as a decision;
//   * each of the other six refusals is reachable.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdint>

#include "src/platform/PpaSrmPlan.h"

namespace {

using ppa::Refusal;
using ppa::SrmFormat;
using ppa::SrmOp;
using ppa::SrmRequest;

// The shipping canvas, exactly: 1216 x 684 at 4 bpp, 608 B per row.
constexpr uint16_t kPanelW = 1216;
constexpr uint16_t kPanelH = 684;
constexpr size_t kPanelRow4 = kPanelW / 2;           // 608
constexpr size_t kPanelBytes4 = kPanelRow4 * kPanelH;  // 415872
constexpr size_t kPanelGray8 = size_t{kPanelW} * kPanelH;  // 831744

// A cache-line-legal PSRAM address to stand in for an aligned allocation.
constexpr uintptr_t kAligned = 0x4FF00000;

struct Case {
  const char* name;
  SrmRequest req;
  bool wantServed;
  Refusal wantWhy;
  // Expected widening cost (0 when the format is byte-addressable).
  size_t wantWidenBytes;
};

SrmRequest base(const SrmFormat f, const SrmOp op, const uint16_t w, const uint16_t h) {
  SrmRequest r;
  r.format = f;
  r.op = op;
  r.width = w;
  r.height = h;
  r.dstWidth = w;
  r.dstHeight = h;
  r.srcStride = (uint32_t)(size_t{w} * ppa::bitsPerPixel(f) / 8);
  r.dstStride = r.srcStride;
  r.dstAddr = kAligned;
  r.dstBytes = (size_t{w} * h * ppa::bitsPerPixel(f)) / 8;
  // Round the stated size up to the cache line, as a real allocation would be.
  r.dstBytes = (r.dstBytes + 63u) & ~size_t{63};
  r.outAlign = 64;
  r.external = true;
  r.flashEncryption = false;
  r.ppaPresent = true;
  return r;
}

const Case kCases[] = {
    // ---- the render path: every surface this tree owns is packed -----------
    {"canvas4bpp_copy", base(SrmFormat::Packed4, SrmOp::Copy, kPanelW, kPanelH), false,
     Refusal::FormatNotAddressable, kPanelGray8},
    {"canvas4bpp_rot180", base(SrmFormat::Packed4, SrmOp::Rot180, kPanelW, kPanelH), false,
     Refusal::FormatNotAddressable, kPanelGray8},
    {"canvas4bpp_mirror_x", base(SrmFormat::Packed4, SrmOp::MirrorX, kPanelW, kPanelH), false,
     Refusal::FormatNotAddressable, kPanelGray8},
    // An ODD width at 4 bpp: the format refusal must still win, because no width
    // makes a nibble buffer addressable.
    {"canvas4bpp_odd_width", base(SrmFormat::Packed4, SrmOp::Copy, 1215, kPanelH), false,
     Refusal::FormatNotAddressable, size_t{1215} * kPanelH},
    // CP_RENDER_4BPP=0: the legacy 1 bpp canvas, 152 B/row.
    {"canvas1bpp_legacy", base(SrmFormat::Packed1, SrmOp::Rot180, kPanelW, kPanelH), false,
     Refusal::FormatNotAddressable, kPanelGray8},
    // The 2 bpp cover thumbnail surface.
    {"thumb2bpp", base(SrmFormat::Packed2, SrmOp::Copy, 600, 900), false,
     Refusal::FormatNotAddressable, size_t{600} * 900},

    // ---- a byte-addressable full frame: served -----------------------------
    {"gray8_frame_copy", base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH), true, Refusal::None, 0},
    {"gray8_frame_mirror_x", base(SrmFormat::Gray8, SrmOp::MirrorX, kPanelW, kPanelH), true, Refusal::None, 0},
    {"gray8_frame_rot180", base(SrmFormat::Gray8, SrmOp::Rot180, kPanelW, kPanelH), true, Refusal::None, 0},
    {"rgb565_mirror_x", base(SrmFormat::Rgb565, SrmOp::MirrorX, 800, 480), true, Refusal::None, 0},

    // ---- a scale is never byte-identical -----------------------------------
    {"rgb565_scale_half",
     [] {
       SrmRequest r = base(SrmFormat::Rgb565, SrmOp::Scale, 800, 480);
       r.dstWidth = 400;
       r.dstHeight = 240;
       r.dstBytes = 400 * 240 * 2;
       return r;
     }(),
     false, Refusal::ScaleNotIdentical, 0},
    {"gray8_scale_2x",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Scale, 100, 100);
       r.dstWidth = 200;
       r.dstHeight = 200;
       r.dstBytes = 200 * 200;
       return r;
     }(),
     false, Refusal::ScaleNotIdentical, 0},

    // ---- driver/SoC refusals ----------------------------------------------
    {"gray8_flash_enc_psram",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH);
       r.flashEncryption = true;
       r.external = true;
       return r;
     }(),
     false, Refusal::ExternalWithFlashEnc, 0},
    {"gray8_flash_enc_internal",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH);
       r.flashEncryption = true;
       r.external = false;
       return r;
     }(),
     true, Refusal::None, 0},
    {"gray8_unaligned_addr",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH);
       r.dstAddr = kAligned + 8;
       return r;
     }(),
     false, Refusal::OutputNotAligned, 0},
    {"gray8_unaligned_size",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH);
       r.dstBytes = kPanelGray8 + 1;
       return r;
     }(),
     false, Refusal::OutputNotAligned, 0},
    {"gray8_undersized_buffer",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH);
       r.dstBytes = 400000;  // 400000 / 64 == 6250 exactly: aligned, but too small
       return r;
     }(),
     false, Refusal::OutputNotAligned, 0},
    {"gray8_tiny", base(SrmFormat::Gray8, SrmOp::Copy, 4, 4), false, Refusal::GeometryOutOfRange, 0},
    {"gray8_oversize", base(SrmFormat::Gray8, SrmOp::Copy, 5000, 100), false, Refusal::GeometryOutOfRange, 0},

    // ---- colour modes that exist but cannot be an output ------------------
    {"yuv444_input_only", base(SrmFormat::Yuv444, SrmOp::Copy, 800, 480), false,
     Refusal::FormatNotAddressable, 0},
    // YUV420 IS a legal input mode, and its out size is 1.5 B/px.
    {"yuv420_copy",
     [] {
       SrmRequest r = base(SrmFormat::Yuv420, SrmOp::Copy, kPanelW, kPanelH);
       r.dstBytes = (size_t{kPanelW} * kPanelH * 3 / 2 + 63) & ~size_t{63};
       return r;
     }(),
     true, Refusal::None, 0},

    // ---- no engine at all -------------------------------------------------
    {"no_ppa_gray8",
     [] {
       SrmRequest r = base(SrmFormat::Gray8, SrmOp::Copy, kPanelW, kPanelH);
       r.ppaPresent = false;
       return r;
     }(),
     false, Refusal::NoPpa, 0},
    {"no_ppa_canvas4bpp",
     [] {
       SrmRequest r = base(SrmFormat::Packed4, SrmOp::Copy, kPanelW, kPanelH);
       r.ppaPresent = false;
       return r;
     }(),
     false, Refusal::NoPpa, kPanelGray8},
};

constexpr int kCaseCount = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));

}  // namespace

// One assertion per case, so a failure names the buffer it is about.
TEST(PpaSrmPlan, EveryTableCase) {
  for (int i = 0; i < kCaseCount; ++i) {
    const Case& c = kCases[i];
    const ppa::SrmPlan p = ppa::planSrm(c.req);
    EXPECT_EQ(p.served, c.wantServed) << "case " << i << " " << c.name;
    EXPECT_STREQ(ppa::refusalName(p.why), ppa::refusalName(c.wantWhy)) << "case " << i << " " << c.name;
    EXPECT_EQ(p.widenBytes, c.wantWidenBytes) << "case " << i << " " << c.name;
    // A served plan never carries a widening step: the formats that need one are
    // refused, and a plan that both widens and serves would be a contradiction
    // the caller could not act on.
    if (p.served) {
      EXPECT_FALSE(p.needsWiden) << "case " << i << " " << c.name;
      EXPECT_EQ(p.why, Refusal::None) << "case " << i << " " << c.name;
    } else {
      EXPECT_NE(p.why, Refusal::None) << "case " << i << " " << c.name;
    }
  }
}

// The counts, printed and asserted. This is the evidence the parent report
// quotes, so it is produced by the binary rather than typed by hand.
TEST(PpaSrmPlan, Counts) {
  int served = 0;
  int refused = 0;
  int perCode[8] = {0};

  for (int i = 0; i < kCaseCount; ++i) {
    const ppa::SrmPlan p = ppa::planSrm(kCases[i].req);
    if (p.served) {
      ++served;
    } else {
      ++refused;
      perCode[static_cast<int>(p.why)]++;
    }
  }

  std::printf("\n--- ppa::planSrm over %d real cases ---\n", kCaseCount);
  for (int k = 1; k <= 6; ++k) {
    std::printf("  %-44s %d\n", ppa::refusalName(static_cast<Refusal>(k)), perCode[k]);
  }
  std::printf("  %-44s %d\n", "SERVED (byte-addressable, identity op)", served);
  std::printf("  %-44s %d\n", "total", kCaseCount);

  EXPECT_EQ(served + refused, kCaseCount);
  // The table's shape: the render path is refused, and "served" is reachable.
  EXPECT_EQ(served, 6);
  EXPECT_EQ(refused, kCaseCount - 6);
  // Every refusal code must be exercised, or the table is not evidence.
  for (int k = 1; k <= 6; ++k) {
    EXPECT_GT(perCode[k], 0) << "refusal " << ppa::refusalName(static_cast<Refusal>(k)) << " unused";
  }
}

// The one case the product actually meets, spelled out: the shipping canvas is
// 4 bpp, and the widening it would need is 831,744 bytes of GRAY8 — twice the
// canvas it would accelerate.
TEST(PpaSrmPlan, ShippingCanvasIsNotAddressable) {
  SrmRequest r = base(SrmFormat::Packed4, SrmOp::Rot180, kPanelW, kPanelH);
  const ppa::SrmPlan p = ppa::planSrm(r);
  ASSERT_FALSE(p.served);
  ASSERT_EQ(p.why, Refusal::FormatNotAddressable);
  EXPECT_TRUE(p.needsWiden);
  EXPECT_EQ(p.widenBytes, 831744u);
  EXPECT_EQ(p.widenBytes, 2u * kPanelBytes4);
}

// The out geometry is derived, not trusted: a caller that passes dstWidth == src
// for a quarter turn still gets the swapped axes reported.
TEST(PpaSrmPlan, QuarterTurnSwapsReportedAxes) {
  SrmRequest r = base(SrmFormat::Gray8, SrmOp::Rot90, kPanelW, kPanelH);
  const ppa::SrmPlan p = ppa::planSrm(r);
  EXPECT_TRUE(p.served);
  EXPECT_EQ(p.outWidth, kPanelH);
  EXPECT_EQ(p.outHeight, kPanelW);
  EXPECT_EQ(p.outBytes, kPanelGray8);  // a permutation moves the same bytes
}

// A mirror moves the same number of bytes it reads.
TEST(PpaSrmPlan, PermutationBytesAreSymmetric) {
  SrmRequest r = base(SrmFormat::Gray8, SrmOp::MirrorX, kPanelW, kPanelH);
  const ppa::SrmPlan p = ppa::planSrm(r);
  EXPECT_TRUE(p.served);
  EXPECT_EQ(p.inBytes, p.outBytes);
}
