#pragma once

// Adapter between CrossPoint's 1-bpp display facade and the vendor EPDiy stack.
// See metalio_vendor_panel.cpp for the contract and the packing details.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
// Called once immediately before and once after every blocking waveform. The panel drive holds the
// caller (the UI task) for the whole refresh -- 483 ms for a DU page turn, up to ~1.8 s for a
// from-white clean -- and the touch controller only reports while a finger is down, so a tap that
// starts and ends inside that window is invisible. Polling at both edges recovers the common case:
// a second tap that lands during the refresh and is still down when it finishes is seen at the exit
// edge and becomes a normal press. NULL = no polling.
typedef void (*MetalioPollHook)(void);
void metalio_vendor_panel_set_poll_hook(MetalioPollHook hook);
void metalio_vendor_panel_set_priority(int priority);

// Forensics hook: called with one formatted line after every show(), carrying the waveform, the
// TPS verdict and the draw result. The app routes it to /.crosspoint/sleep.txt -- the only store
// that survives the power-off, since RTC_NOINIT RAM is wiped by the POWERON reset. NULL = silent.
typedef void (*MetalioJournalHook)(const char* line);
void metalio_vendor_panel_set_journal_hook(MetalioJournalHook hook);

// r129 (H23): drive gate. Called with active=1 immediately before a waveform starts clocking and
// active=0 as soon as it returns, for the whole drive -- including the 0x400 recovery path that
// follows a failed one. The app's sink holds the SD log writer for that window (logSetDriveActive),
// so no flash erase can land between two panel lines. NULL = no gate, and a sink that is already
// idle is a no-op, so this can only ever prevent a write, never add one.
typedef void (*MetalioDriveGateHook)(int active);
void metalio_vendor_panel_set_drive_gate_hook(MetalioDriveGateHook hook);

// The vendor's own reset gap: power off, hold ~20 ms, power back on. A waveform measured at 48
// phases still leaving the previous frame on glass means the rails were not actually asserted,
// and only a real TPS65185 cycling makes that claim testable. Returns the TPS verdict after it.
bool metalio_vendor_panel_repower(void);

// ---------------------------------------------------------------------------
// READ-ONLY TPS65185 register readout for the USB shell's `pmic` / `panel` commands.
//
// This lives in the driver rather than in the app on purpose. Two reasons:
//   1. The PMIC's registers are this component's business — the shell only wants text,
//      so the app never has to carry a raw `bus/tps65185.h` dependency. (Such a
//      dependency does not link anyway: the app's probe TU is C++ while those
//      functions have C linkage, so the reference is mangled and no archive member
//      can satisfy it. Keeping the read behind this shim is the fix, not a workaround.)
//   2. Every access here is tps_read_register_ex(), which reports the I2C result, so a
//      NAK is printed as a failure instead of being flattened into a plausible 0x00.
//
// Read-only by construction: nothing in this path writes a register.
//
// PMIC-ASLEEP TRAP: the TPS65185 does not ACK I2C in SLEEP, and the board parks it
// there after every drive and on standby. The dump therefore WAKEs the part to
// STANDBY (WAKEUP high, PWRUP/VCOM low) and restores the exact power state it
// found -- it never raises PWRUP, so a debug command cannot leave the rails up.
// was_asleep/woken/wake_ms say what happened so the caller can report it honestly.
typedef struct {
  int total;         // registers in the table
  int failed;        // reads that returned an I2C error
  int pg_ok;         // 1 when the PG read succeeded
  unsigned char pg;  // last PG value read (meaningful only when pg_ok)
  int was_asleep;    // 1 when the PMIC was in SLEEP (no I2C) before the read
  int woken;         // 1 when the PMIC answered after WAKEUP (or was already awake)
  int wake_ms;       // ms spent waiting for the part to answer after WAKEUP
} MetalioTpsDump;

// One "  0x0F PG          = 0xFA  250" line per register into `out` (always NUL-
// terminated, never writes past `cap`). Fills `summary` when non-NULL. Returns
// summary->total. Read-only.
int metalio_vendor_panel_tps_dump(char* out, size_t cap, MetalioTpsDump* summary);

// PG alone, for the `panel` command. Returns 0 on success, otherwise the I2C
// error (an esp_err_t) as int. Read-only.
int metalio_vendor_panel_tps_read_pg(unsigned char* pg);

// ---------------------------------------------------------------------------
// Fault telemetry (3.9): the PMIC's interrupt/fault flags, decoded.
//
// READ-ONLY BY CONSTRUCTION: no register is written here, no latch is cleared by
// a write, and the exact power state found is restored afterwards (same wake-then-
// restore contract as the dump above -- it never raises PWRUP).
//
// ONE HONEST CAVEAT: on this part a register READ is also the acknowledge, and this
// firmware already relies on that -- bus/tps65185.c tps_vcom_kickback_start() does a
// throwaway INT1 read before arming a kick-back measurement, which is only sensible
// if reading clears. So a sample returns the flags that were PENDING UP TO that read
// and consumes them; the sample itself is the record. Print it, log it, do not
// re-read to "double check" -- the second read says nothing happened.
//
// Bit names are NOT guessed. INT1 carries only the bits this tree can name: 7..3
// from the vendor-derived decoder in bus/tps65185.c debug_tps_error(), and bit 1
// (ACQC) from tps_vcom_kickback_rdy(), which tests INT1 == 0x02. Bits 0 and 2, and
// the whole of INT2, have no bit map in this tree and are reported as raw hex.
typedef struct {
  int int1_ok;         // 1 when INT1 answered
  int int2_ok;         // 1 when INT2 answered
  unsigned char int1;  // raw INT1 (meaningful when int1_ok)
  unsigned char int2;  // raw INT2 (meaningful when int2_ok)
  int pg_ok;           // 1 when PG answered
  unsigned char pg;
  int named_bits;      // how many INT1 bits this tree can name
  int was_asleep;      // PMIC was in SLEEP (no I2C) before the sample
  int woken;           // it answered after WAKEUP
  int wake_ms;
} MetalioTpsFaults;

// Short token for one INT1 bit, or NULL when the bit has no name in this tree
// ("VCOM", "VNEG", "VPOS", "VGL", "VGH", "ACQC"). The SINGLE copy of the map
// lives in the driver so the shell text and the Settings row cannot disagree.
const char* metalio_vendor_panel_tps_int1_short(int bit);

// One sample. Appends a decoded block to `text` (NUL-terminated, never past
// `cap`; pass NULL/0 for no text) and fills `out` when non-NULL. Returns 0 when
// INT1 was read, otherwise the I2C error (esp_err_t) as int.
int metalio_vendor_panel_tps_faults(MetalioTpsFaults* out, char* text, size_t cap);

// ---------------------------------------------------------------------------
// Panel VCOM / black depth (D9).
//
// STORE ONLY -- this touches no register, which is precisely why it is safe to
// call from a settings handler. The value is what the power-on sequence writes
// (epd_board_metalio_eink4_plus.c epd_board_poweron() -> tps_set_vcom(vcom)),
// i.e. rails ramping, output enable still LOW, no waveform clocking: the one
// window where a VCOM change cannot land in the middle of a scan. Refuses
// anything outside the vendor's three levels {1200, 1600, 2000} mV and returns
// a negative value (-EINVAL) without changing the stored one.
int metalio_vendor_panel_set_vcom_mv(int mv);

// The value the next power-on will write (no bus access).
int metalio_vendor_panel_vcom_mv(void);
#endif

// Initialise the vendor stack and whiten the glass once. Returns false if the
// high-level framebuffers could not be allocated (PSRAM problem).
bool metalio_vendor_panel_begin(void);

// One-shot, armed BEFORE metalio_vendor_panel_begin(): skip the boot clear.
// Legal only when the caller guarantees that the first paint of this boot is a
// whole-panel forced drive (epd_hl_update_screen_full clears MODE_SKIP_UNCHANGED,
// so every pixel is conditioned) -- the software baseline comes from epd_hl_init()'s
// memset(0xFF), never from the glass, so a guaranteed full drive makes the clear
// redundant. Used on a silent reboot, where the glass is carrying the "Loading…"
// popup the previous session drove 50 ms before the reset: the clear would erase it
// and leave ~810 ms of white before the arriving screen, which is the blank frame
// the operator reads as "the device is doing nothing".
void metalio_vendor_panel_keep_glass_on_boot(void);

// Refresh intent for one panel push. The three intents select three DIFFERENT waveforms, and
// collapsing them into a bool is what made every partial update run the 48-phase grayscale table:
//   FAST -> MODE_DU   (20 phases, binary, no grey flash)  -- page turns, UI updates
//   HALF -> MODE_GC16 (48 phases)                          -- the periodic ghost-cleaning refresh
//   FULL -> MODE_GC16 (48 phases)                          -- sleep screen / explicit full
typedef enum {
  METALIO_PANEL_REFRESH_FAST = 0,
  METALIO_PANEL_REFRESH_HALF = 1,
  METALIO_PANEL_REFRESH_FULL = 2,
  // 16-level grayscale waveform. In THIS table it is also 48 frames -- the difference from GC16 is
  // content, not length: gl16_white_tick() drives the 15->15 transition, so already-white areas get
  // pushed clean instead of being left alone. Exposed so the three can be compared on the glass.
  METALIO_PANEL_REFRESH_GL16 = 3,
  // Whole-page black re-ink (vendor v2.0.74 MODE_EQUALIZE_BLACK, upstream 2b2c36a). Same
  // 48-frame GC16 table as HALF, but every pixel whose TARGET is black is driven black
  // whatever it currently shows, and the drive covers the whole panel instead of the
  // changed rectangle. The page still ends on its new content -- this is not an erase.
  METALIO_PANEL_REFRESH_EQUALIZE = 4,
} MetalioPanelRefresh;

// Convert one 1-bpp frame (bit set = WHITE, MSB first) and push it with `refresh`'s waveform.
void metalio_vendor_panel_show(const uint8_t* bw, int w, int h, int stride, int refresh,
                               bool power_off_after);


// Drop the rails if they were raised.
void metalio_vendor_panel_off(void);

// ---------------------------------------------------------------------------
// VID1 — the 8-phase short-DU bench lane (r101.21).
//
// `frames` selects idf/components/metalio_driver/display/waveforms/my_waveform.c's
// already-built short_du table (my_waveform_short_du(), 8 phases, ~96 ms at the
// 12 ms floor) for that many FAST paints, then reverts by itself. EACH selected
// paint is a WHOLE-PANEL drive (MODE_SKIP_UNCHANGED cleared), so the log line is
// a waveform measurement and not the diff-scan no-op LOG1 warns about. One line
// per frame: `[VID] frame i/N lane=shortDU(8) ms=<n> err=0x<..>`.
//
// frames <= 0 disarms. The counter is a file-static initialised to 0, so with no
// command typed this file behaves exactly as before r101.21 -- the shipped FAST
// lane still selects the 20-phase DU table.
void metalio_vendor_panel_short_du_frames(int frames);
int metalio_vendor_panel_short_du_remaining(void);

// Draw result of the last metalio_vendor_panel_show(): 0 = the drive ran and the
// waveform reported success. Anything else is the panel's own EpdDrawError (or, for
// 0x40000000, a show() that refused before driving). Consumed by the renderer's
// refresh-policy latch (RE_RICKYOS_PIPELINE.md rows 7 and 12).
uint32_t metalio_vendor_panel_last_draw_error(void);

int metalio_vendor_panel_width(void);
int metalio_vendor_panel_height(void);

#ifdef __cplusplus
}
#endif

/* One bus, one lock. Board-level code that performs a MULTI-STEP Wire sequence
 * (write register, then read back) must bracket it with these, because the vendor
 * shims take the same mutex and app-side Wire users take none. Recursive, so a shim
 * called inside a locked region will not deadlock. */
#ifdef __cplusplus
extern "C" {
#endif
bool mn_bus_lock(uint32_t timeout_ms);
void mn_bus_unlock(void);

/* Recover a stuck I2C bus. MUST be called BEFORE Wire.begin(), never after. */
void mn_bus_recover(void);

/* Haptics: the vibration motor hangs off the TCA9555 at P0.3 (vendor
 * config.h:68 EPD_IO_MOTOR 3, active high; config.h:72 VIBRATION_MOTOR_PULSE_MS
 * 50 for the short "click"). One bounded pulse: motor on, delay(ms), motor off,
 * then return -- no timer and no background state, so an interrupted caller
 * cannot leave the motor energised. Implemented in metalio_periph.c on top of
 * the vendor board API (epd_board_metalio_eink4_plus_motor_set), which is a
 * no-op when the expander did not answer. Blocks the caller for `ms`.
 * This is the fixed-width, full-strength pulse: mn_haptic_pulse_level(ms, 3). */
void mn_haptic_pulse(int ms);

/* Strength-carrying pulse (r89, corrected in r90). `level` is 0..3 as in the
 * vendor's table (metalio_vibe.cc k_duty_on_us[]); `ms` is a DURATION FLOOR,
 * not the whole story -- each level also owns a period and a minimum duration,
 * because r89 shipped only the vendor's duty over the vendor's 50 ms pulse and
 * every level felt the same (see the long comment above the implementation in
 * metalio_periph.c for the three reasons). Blocks the caller; writes the motor
 * bit low on every exit path including a dead expander; never leaves a timer
 * armed. Logs the measured on/off timings at INFO so the achieved duty can be
 * checked on the device. */
void mn_haptic_pulse_level(int ms, int level);
#ifdef __cplusplus
}
#endif
