// # r106 B1 — the pure TCA port bit arithmetic.
//
// This header is what the BOARD actually includes (the board layer is C, so the header
// is C-compatible). The properties proven here are the ones the glass log will show:
//   * a cut clears EXACTLY the named bit and leaves every other output bit as it was;
//   * a restore sets EXACTLY the named bit;
//   * the byte is a pure function of (current, bit, on) -- no board latches involved;
//   * the path the caller took (read vs fallback shadow) is reported, not implied.
// The failure this guards (R103_GLASS_LOG §F1) was a write of 0x00 where the part held
// other output bits: every "other bits survive" case below is that defect turned red.

#include "platform/TcaRmw.h"

#include <gtest/gtest.h>

namespace {

constexpr uint8_t CAM = 0x20;  // P0.5 CAM_SCR_EN (the bit `trim cam cut` drives)

uint8_t setBit(uint8_t cur, uint8_t bit) { return tca_rmw_apply_bit(cur, bit, 1, TCA_RMW_PATH_READ).value; }
uint8_t clearBit(uint8_t cur, uint8_t bit) { return tca_rmw_apply_bit(cur, bit, 0, TCA_RMW_PATH_READ).value; }

TEST(TcaRmw, CutClearsOnlyTheNamedBit) {
  // 0x3F = every port-0 output high (VCOM/PWRUP/WAKEUP/MOTOR/PA_EN/CAM). This is the
  // exact byte the r105 code failed to write: it wrote 0x00 instead of 0x1F.
  EXPECT_EQ(clearBit(0x3F, CAM), 0x1F);
  EXPECT_EQ(clearBit(0x3F, CAM) != 0x00, true);
}

TEST(TcaRmw, RestoreSetsOnlyTheNamedBit) {
  EXPECT_EQ(setBit(0x1F, CAM), 0x3F);
  EXPECT_EQ(setBit(0x00, CAM), 0x20);
  EXPECT_EQ(setBit(0x20, CAM), 0x20);  // already set: idempotent
}

TEST(TcaRmw, OtherBitsSurviveEveryCombination) {
  for (int cur = 0; cur < 256; ++cur) {
    const uint8_t byte = static_cast<uint8_t>(cur);
    const uint8_t off = clearBit(byte, CAM);
    const uint8_t on = setBit(byte, CAM);
    // Only bit 5 may differ, in either direction.
    EXPECT_EQ(off & static_cast<uint8_t>(~CAM), byte & static_cast<uint8_t>(~CAM)) << "cur=0x" << cur;
    EXPECT_EQ(on & static_cast<uint8_t>(~CAM), byte & static_cast<uint8_t>(~CAM)) << "cur=0x" << cur;
    EXPECT_EQ((off & CAM) != 0, false) << "cur=0x" << cur;
    EXPECT_EQ((on & CAM) != 0, true) << "cur=0x" << cur;
  }
}

TEST(TcaRmw, PathIsReportedNotInferred) {
  EXPECT_EQ(tca_rmw_apply_bit(0x3F, CAM, 0, TCA_RMW_PATH_READ).path, TCA_RMW_PATH_READ);
  EXPECT_EQ(tca_rmw_apply_bit(0x3F, CAM, 0, TCA_RMW_PATH_SHADOW).path, TCA_RMW_PATH_SHADOW);
  EXPECT_STREQ(tca_rmw_path_name(TCA_RMW_PATH_READ), "rmw");
  EXPECT_STREQ(tca_rmw_path_name(TCA_RMW_PATH_SHADOW), "shadow");
}

TEST(TcaRmw, FallbackShadowIsTheSameArithmetic) {
  // The shadow path must not "repair" anything: same byte in, same byte out.
  EXPECT_EQ(tca_rmw_apply_bit(0x3F, CAM, 0, TCA_RMW_PATH_SHADOW).value, 0x1F);
  EXPECT_EQ(tca_rmw_apply_bit(0x1F, CAM, 1, TCA_RMW_PATH_SHADOW).value, 0x3F);
}

}  // namespace
