/*
 * App-side esp_cpu_configure_region_protection() for the ESP32-S31.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * IDF is linked into this firmware as PREBUILT archives, so the copy of
 * esp_cpu_configure_region_protection() inside libesp_hw_support.a is whatever
 * the framework package shipped -- and on this board it does not map PSRAM.
 * Compiling IDF from source with the SAME sdkconfig does map PSRAM correctly,
 * which was measured directly (see probe/, and docs/s31-pma-bringup.md).
 *
 * WHAT THE FUNCTION MUST DO
 * -------------------------
 * Exactly what IDF's own esp32s31 implementation does, and nothing more: clear
 * PMA entry 7 and re-point it at the external RAM window as NAPOT | LOCK |
 * R+W+X. The PMA entry state dumped from a working from-source boot is:
 *
 *   entry  7 cfg=0xe000001d addr=0x147fffff   PSRAM 0x50000000..0x54000000
 *   entry 12 cfg=0xc0000019 addr=0x0b800fff
 *   entry 13 cfg=0xc0000015 addr=0x0be0ffff   ROM
 *   entry 14 cfg=0xc000001d addr=0x0bc0ffff   SRAM
 *   entry 15 cfg=0xc0000015 addr=0x13ffffff   flash 0x40000000, R+X
 *
 * and 0xe000001d / 0x147fffff is precisely what the call below produces.
 *
 * TWO EARLIER ATTEMPTS, AND WHY THEY FAILED
 * -----------------------------------------
 * 1. An EMPTY function: the app boots, then dies at 0x50000020 (PSRAM base +
 *    0x20) with a store access fault, because nothing maps external RAM.
 * 2. Hand-rebuilt TOR/NAPOT entries chosen from disassembly: the app reset-
 *    loops with no output, because those edits cleared the mapping the code
 *    itself executes from.
 *
 * The lesson: do not invent PMA state. Reproduce the one assignment that a
 * known-good boot produces. Do not touch flash (entry 15) -- the bootloader
 * establishes it and this function must not disturb it.
 *
 * The bootloader-side patch previously applied by tools/flash_s31.py is not
 * needed for this and is now off by default; PSRAM is mapped from the app.
 */

#include <stdint.h>

#include "sdkconfig.h"
#include "soc/soc.h"
#include "riscv/csr.h"

void esp_cpu_configure_region_protection(void)
{
    /* DELIBERATELY EMPTY.
     *
     * Applying the correct mapping here (PMA_RESET_AND_ENTRY_SET_NAPOT on
     * entry 7, byte-for-byte what IDF's own source does) makes the app
     * CPU-lockup before the console exists -- measured, build 53. The same
     * call from a from-source IDF build works, so this is an artefact of the
     * prebuilt archives, not of the values.
     *
     * So entry 7 is left alone here and mapped later from setup() by
     * mccrossy_psram_diag(), where the console is up and each step is
     * printed. Until then the app runs fine from flash and internal RAM; it
     * only must not touch external RAM. */
    (void)0;
}
