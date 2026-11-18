#pragma once

// Part names the About screen prints for the Plus panel chain, in ONE place.
//
// Why they are here and not read from a header: nothing in the tree carries them as
// a string. The vendor driver identifies the panel by the C SYMBOL it links
// (&ED047TC2_1216, idf/components/metalio_driver/display/displays.c:67) and the
// board's PMIC/expander by their I2C address constants (TPS65185_ADDR /
// EXPANDER_ADDR in freeink-sdk's MetalioEInk4PlusBoard.h), so there is no macro to
// include -- only a symbol name and a comment. Spelling them once here, next to the
// symbols they name, is what keeps a future panel revision to a one-file change.
//
// The numbers printed BESIDE these names are not duplicated: geometry comes from
// BoardConfig::ACTIVE (runtime board profile) and the addresses come from
// metalio4plus::* in the board header, so if this file ever disagrees with the
// hardware the About row shows the mismatch instead of hiding it.

#if FREEINK_DEVICE_METALIO_EINK4_PLUS

#define CP_PANEL_CONTROLLER_NAME "ED047TC2_1216"  // displays.c:67 ED047TC2_1216 (1216x684, 8-bit parallel)
#define CP_PANEL_VENDOR_PART "E0470A14-AF"        // displays.c:76 E0470A14_AF rev A, 34-pin
#define CP_PANEL_PMIC_NAME "TPS65185"             // bus/tps65185.h; address = metalio4plus::TPS65185_ADDR
#define CP_PANEL_PMIC_EXPANDER "TCA9555"          // bus/pca9555.h driver; address = metalio4plus::EXPANDER_ADDR

#endif
