// Panel + PMIC probes for the USB shell (see PanelProbe.h).
//
// READ-ONLY BY CONSTRUCTION. The only write path in this file is none: every
// TPS65185 access goes through metalio_vendor_panel_tps_*() in the driver (which
// reads with tps_read_register_ex()), every expander access is
// epd_board_*_ioexp_get_level()/read_port(). The `refresh` and `shot` commands
// deliberately do NOT live here — they go through the UI's own path in main.cpp
// so RefreshPolicy and RenderLock still decide.

#include "debug/PanelProbe.h"

#include "platform/PlatformFeatures.h"

#if CP_USB_SHELL

#include <Arduino.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if FREEINK_DEVICE_METALIO_EINK4_PLUS

#include <esp_err.h>

#include "display/epd_board_metalio_eink4_plus.h"
#include "metalio_vendor_panel.h"  // metalio_vendor_panel_*: draw error, TPS register readout
#include "debug/UsbShell.h"        // lastPanelJournal()

namespace {

// P0.7 on the TCA9555 is the TPS65185's PWR_GOOD input (vendor
// main/boards/metalio-e-ink4-plus/config.h:68  EPD_TPS_IO_PWR_GOOD 7).
// Declared here rather than including that config.h: several IDF components ship
// a bare `config.h` on the include path, and picking up the wrong one would
// silently change the pin.
constexpr int kPwrGoodIo = 7;

void appendf(char* out, size_t cap, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void appendf(char* out, size_t cap, const char* fmt, ...) {
  const size_t len = strlen(out);
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out + len, cap - len, fmt, ap);
  va_end(ap);
}

}  // namespace

namespace panelprobe {

int panelText(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return -EINVAL;
  out[0] = '\0';

  // The vendor shim already emits one journal line per show():
  //   [panel] show mode=GC16-from-white tps=1 err=0x0 ms=..
  // That IS the last RESOLVED mode (not the requested one) plus that drive's own
  // TPS verdict, so it is quoted rather than re-derived.
  const char* last = usbshell::lastPanelJournal();
  appendf(out, cap, "resolved: %s\n", (last != nullptr && last[0] != '\0') ? last : "(no drive since boot)");

  const uint32_t err = metalio_vendor_panel_last_draw_error();
  appendf(out, cap, "draw_err: 0x%lx %s\n", static_cast<unsigned long>(err),
          err == 0 ? "(success)"
                   : err == 0x40000000UL ? "(show refused before driving)" : "(panel EpdDrawError)");

  const bool ioexp = epd_board_metalio_eink4_plus_ioexp_ok();
  const bool tpsOk = epd_board_metalio_eink4_plus_tps_ok();
  appendf(out, cap, "latch: tps_ok=%d ioexp_ok=%d  (these are the sticky verdicts, not live reads)\n",
          tpsOk ? 1 : 0, ioexp ? 1 : 0);

  // PWR_GOOD is a TCA9555 INPUT (P07) driven by the TPS65185; reading it does
  // not disturb the PMIC.
  const bool pgood = epd_board_metalio_eink4_plus_ioexp_get_level(kPwrGoodIo);
  uint8_t p0in = 0, p1in = 0;
  const bool p0ok = epd_board_metalio_eink4_plus_ioexp_read_port(0, &p0in);
  const bool p1ok = epd_board_metalio_eink4_plus_ioexp_read_port(1, &p1in);
  appendf(out, cap, "pwr_good: %d (P07 input) port0_in=0x%02X port1_in=0x%02X", pgood ? 1 : 0,
          p0ok ? p0in : 0, p1ok ? p1in : 0);
  if (!p0ok || !p1ok) appendf(out, cap, " [read FAILED]");
  appendf(out, cap, "\n");

  unsigned char pgVal = 0;
  const int pgErr = metalio_vendor_panel_tps_read_pg(&pgVal);
  if (pgErr == 0) {
    appendf(out, cap, "pg_reg: 0x%02X rails_up=0x%02X (vendor wants (PG&0xFA)==0xFA)\n", pgVal,
            static_cast<unsigned>(pgVal & 0xFA));
    if (pgVal == 0 && pgood) {
      // The two sources disagree, and that is worth naming rather than leaving to a reader:
      // see PG1 in GfxRenderer (the 'frame did not land' check). PG is only written/readable
      // inside the power-up window; outside it a parked part answers 0x00 while PWR_GOOD
      // (open-drain, released when the part is asleep) floats high. So this line is NOT
      // evidence that the rails are down -- the detect path samples PWR_GOOD at the end of a
      // waveform instead, which is the only window where the pin means anything.
      appendf(out, cap,
              "  note: PG=0x00 with PWR_GOOD=1 is a parked/non-reporting PMIC, not a rail-down "
              "reading; the rail check samples PWR_GOOD at the end of a drive\n");
    }
  } else {
    appendf(out, cap, "pg_reg: I2C error %s\n", esp_err_to_name(static_cast<esp_err_t>(pgErr)));
  }

  appendf(out, cap, "last_drive: %lu ms (entry->exit of the last blocking waveform)\n",
          usbshell::lastPanelDriveMs());
  return 0;
}

int pmicText(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return -EINVAL;
  out[0] = '\0';

  // The register table and the read path both live in metalio_driver: the shell
  // prints this, but the PMIC registers are the driver's business, so the app
  // carries no raw bus/ dependency (and a C++ TU cannot link those C symbols
  // directly — see metalio_vendor_panel.h).
  appendf(out, cap, "TPS65185 @0x68 on I2C1 — READ ONLY (no register written)\n");
  MetalioTpsDump dump{};
  const int total = metalio_vendor_panel_tps_dump(out, cap, &dump);
  // The PMIC is parked in SLEEP after every drive and on standby, where it does not
  // ACK I2C at all. The driver wakes it to STANDBY for the read and puts the power
  // state back; say which of those happened instead of leaving the operator to
  // guess whether a NAK means "asleep" or "bus broken".
  if (dump.was_asleep) {
    if (dump.woken) {
      appendf(out, cap,
              "power: PMIC was ASLEEP (rails down) — WAKEUP asserted, it answered after %d ms; "
              "original power state restored (rails back down)\n",
              dump.wake_ms);
    } else {
      appendf(out, cap,
              "power: PMIC is ASLEEP and did not answer within %d ms of WAKEUP — rails were left "
              "DOWN (unchanged). Re-run after a page turn or a `refresh` if you need a live read.\n",
              dump.wake_ms);
    }
  } else if (dump.pg_ok) {
    appendf(out, cap, "power: PMIC already awake (PWRUP/WAKEUP asserted) — power state untouched\n");
  }
  if (dump.pg_ok) {
    appendf(out, cap, "decoded: PG=0x%02X rails_up=0x%02X (vendor wants 0xFA)\n", dump.pg,
            static_cast<unsigned>(dump.pg & 0xFA));
  } else {
    appendf(out, cap, "decoded: PG unreadable — rail state unknown (see the error above)\n");
  }
  appendf(out, cap, "         INT1/INT2 non-zero bits mean an event is pending and unread\n");
  // 3.9: the fault flags, with names. A SEPARATE sample from the dump above on purpose --
  // the read is the acknowledge on this part, so the dump's raw INT1 is already stale by
  // the time this runs. This sample is the one that gets reported.
  {
    MetalioTpsFaults faults{};
    metalio_vendor_panel_tps_faults(&faults, out, cap);
  }
  if (dump.failed > 0) {
    appendf(out, cap, "WARNING: %d of %d register(s) did not ACK — see 'power:' above for whether "
                      "the PMIC was simply asleep\n",
            dump.failed, total);
  }
  return (total > 0 && dump.failed == total) ? -EIO : 0;
}

}  // namespace panelprobe

#else  // !FREEINK_DEVICE_METALIO_EINK4_PLUS

namespace panelprobe {
int panelText(char* out, size_t cap) {
  if (out && cap) snprintf(out, cap, "panel: not a Metalio E-INK 4 Plus build\n");
  return -ENODEV;
}
int pmicText(char* out, size_t cap) {
  if (out && cap) snprintf(out, cap, "pmic: not a Metalio E-INK 4 Plus build\n");
  return -ENODEV;
}
}  // namespace panelprobe

#endif  // FREEINK_DEVICE_METALIO_EINK4_PLUS

#else  // !CP_USB_SHELL

namespace panelprobe {
int panelText(char*, size_t) { return -ENODEV; }
int pmicText(char*, size_t) { return -ENODEV; }
}  // namespace panelprobe

#endif  // CP_USB_SHELL
