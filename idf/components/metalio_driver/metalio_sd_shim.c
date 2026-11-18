// Minimal, faithful replacement for the one symbol the vendor's board glue needs from
// metalio_sd.cc (which cannot build here: it pulls SdCardManager.hpp from the vendor app).
//
// epd_board_metalio_eink4_plus.c:359 calls MetalioSd_ReleaseHold() during panel power-on to undo
// any gpio_hold on the shared SDMMC pins. In metalio_sd.cc that function is nothing but a
// gpio_hold_dis() loop -- no SdCardManager dependency at all -- so this is the same code, not a
// stub. Semantics identical: release every SD pin from its sleep hold, return nothing.
#include "config.h"

#include <driver/gpio.h>

static const gpio_num_t kSdPins[] = {
    SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN, SDMMC_D1_PIN, SDMMC_D2_PIN, SDMMC_D3_PIN,
};

void MetalioSd_ReleaseHold(void) {
    for (unsigned i = 0; i < sizeof kSdPins / sizeof kSdPins[0]; ++i) {
        gpio_hold_dis(kSdPins[i]);
    }
}
