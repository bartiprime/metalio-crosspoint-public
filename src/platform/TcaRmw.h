#pragma once

// ---------------------------------------------------------------------------
// TCA PORT READ-MODIFY-WRITE — r106 patch B1 (docs/engineering/R103_GLASS_LOG.md §F1).
//
// WHY THIS EXISTS
//   `trim cam cut` on r105 logged `P05 CAM/SCR rail off (P0=0x00)` and the device
//   then aborted sdmmc (0x109/0x107/0x108) and panic-rebooted: the board helper
//   wrote the WHOLE port-0 output byte from a shadow built by p0_output_value(),
//   and that byte had every other output bit at 0. A single-bit rail cut must
//   never write a byte it did not first READ from the part: TCA9555 output port 0
//   is register 0x02, port 1 is 0x03 (the INPUT ports at 0x00/0x01 are not it).
//
//   The vendor firmware does the same thing through a shadow that still holds the
//   other pins, which is why its 60 s rail cycling survives in the field.
//
// THIS FILE IS PURE AND C-COMPATIBLE: no Arduino, no IDF, no I2C. The board layer
// (idf/components/metalio_driver/display/epd_board_metalio_eink4_plus.c) is a C
// translation unit and includes this directly, and test/tca_rmw compiles the same
// header on the host, so the arithmetic below is literally the code the device runs.
// ---------------------------------------------------------------------------

#include <stdint.h>

// Where the "current" byte came from. Printed in every rail log line, so a
// failure on glass says which path it took instead of implying the other one.
#define TCA_RMW_PATH_READ 0    // the TCA's own output register answered
#define TCA_RMW_PATH_SHADOW 1  // the read failed; the caller's full port shadow was used

typedef struct {
  uint8_t value;  // the byte to write
  int path;       // TCA_RMW_PATH_READ | TCA_RMW_PATH_SHADOW
} tca_rmw_plan_t;

// Clear/set EXACTLY `bit` (a one-hot mask) and leave every other bit as it was.
// `current` is the byte read back from the output register (path READ) or the
// caller's shadow (path SHADOW). Nothing else is inferred: an output bit the mask
// does not name can never change here.
static inline tca_rmw_plan_t tca_rmw_apply_bit(const uint8_t current, const uint8_t bit, const int on,
                                               const int path) {
  tca_rmw_plan_t p;
  p.path = path;
  p.value = on ? (uint8_t)(current | bit) : (uint8_t)(current & (uint8_t)~bit);
  return p;
}

// The name the log line prints for a path: "rmw" | "shadow".
static inline const char* tca_rmw_path_name(const int path) {
  return path == TCA_RMW_PATH_READ ? "rmw" : "shadow";
}
