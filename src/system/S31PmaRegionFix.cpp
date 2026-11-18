// Fix for the ESP32-S31 early-boot crash in IDF's esp_cpu_configure_region_protection().
//
// SYMPTOM
//   Every application built with framework = arduino dies before setup():
//       rst:0x1a (CPU_LOCKUP_RESET)
//       PCAddr: <esp_cpu_configure_region_protection + 8>  Cause: 0x1 (instr access fault)
//       PCAddr: 0x2f000000 (_vector_table)                 -> double fault -> bootloop
//   A 20-line sketch fails identically, so it is not application code.
//
// ROOT CAUSE
//   The vendor ships this function PRECOMPILED in
//       framework-arduinoespressif32-libs/esp32s31/lib/libesp_hw_support.a
//   and its first act is to *zero* PMA entry 7 before rewriting it:
//       csrwi 0xbc7, 0        // PMACFG7  = 0
//       csrwi 0xbd7, 0        // PMAADDR7 = 0
//       lui   a5, 0x14800 ... // PMAADDR7 = 0x147FFFFF  (NAPOT 0x50000000..0x54000000)
//       lui   a5, 0xe0000 ... // PMACFG7  = 0xE000001D
//   On this SoC/layout, that zeroing step revokes EXECUTE permission for the flash region
//   the function is itself running from, so the very next instruction fetch faults at +8.
//   The identical instruction sequence is harmless in a framework = espidf build, which is
//   why the ESP-IDF path boots and the Arduino path bootloops (both verified on hardware).
//
// FIX
//   Define the symbol in the application. A strong definition in our objects means the
//   linker never pulls cpu_region_protect.c.o out of the archive, and we write the entry
//   directly -- no zeroing -- with the exact encoding the vendor's own object uses
//   (disassembly-verified). Verified on hardware: boots, psram found=1, size=16777216.
//
// SCOPE
//   Compiled only for S31; harmless elsewhere because the symbol is only called by the
//   S31 start-up path. If IDF is ever built from source for this target, revisit: the
//   upstream function may be fixed and this override would shadow it.

#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CROSSPOINT_S31_PMA_FIX_FORCE)

extern "C" void esp_cpu_configure_region_protection(void) {
  // PMACFG7/PMAADDR7 for the 64 MB external-RAM window at 0x50000000.
  // NAPOT address field = (START | ((SIZE - 1) >> 1)) >> 2 = 0x51FFFFFF >> 2 = 0x147FFFFF.
  // Config value 0xE000001D is copied from the vendor's own object.
  // Address first, then config (matches the vendor's order after its zeroing step).
  __asm__ volatile("csrw 0xbd7, %0" ::"r"(0x147FFFFFu));
  __asm__ volatile("csrw 0xbc7, %0" ::"r"(0xE000001Du));
}

#else

// Other targets keep IDF's implementation; nothing to override.
extern "C" void crosspoint_s31_pma_fix_unused_anchor(void) {}

#endif
