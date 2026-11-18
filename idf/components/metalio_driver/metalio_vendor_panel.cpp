// Metalio E-Ink 4 Plus — vendor EPDiy panel, wired to CrossPoint's 1-bit facade.
//
// WHY THIS EXISTS
//   CrossPoint's FreeInkDisplay facade composes a 1-bpp framebuffer
//   (1216 x 684, bit set = WHITE, stride = 152 bytes) and hands it to a
//   PanelDriver. The vendor stack (components/metalio_driver) drives the same
//   panel through epdiy's high-level API, which works in 4 bpp (nibble 0xF =
//   WHITE, 0x0 = BLACK) with 608 bytes per row.
//
//   This module is the adapter: it owns the vendor stack's lifecycle and
//   converts one 1-bpp frame into the vendor framebuffer before pushing it.
//
// I2C OWNERSHIP (the rule that took days to learn)
//   Arduino's Wire is the ONLY I2C master on SDA=0/SCL=1, begun once in setup()
//   before this file runs. The vendor shims (pca9555.c, tps65185.c) transfer
//   through mn_wire_* below. Nothing here may create a second master: the master
//   that initialises second cannot ACK, which shows up as "TCA9555 not found"
//   -> "poweron skipped" -> a blank panel.
//   Bus recovery happens BEFORE Wire.begin(), never after: gpio_config() on
//   GPIO0/1 detaches them from the peripheral and every transfer then fails.

#include "metalio_vendor_panel.h"

#include <Arduino.h>
#include <Wire.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

// r97 Phase 2: default for the LUT-format A/B. The CMake option METALIO_EPD_LUT_64K
// defines this to 1 for the scratch build; every normal build leaves it undefined and
// gets the 1 KB LUT this fork has always shipped.
#ifndef METALIO_EPD_LUT_64K
#define METALIO_EPD_LUT_64K 0
#endif

// ---------------------------------------------------------------------------
// r156 (B1) — CP_UI_GREY_CLEAN_EVERY: UI grey-paint cadence.
//
// THE DEFECT. AA/2-bit text paints through the grey lane (`useGray8` below ->
// my_waveform_wipe_gl(3), 38 phases). That lane is a MULTI-LEVEL waveform: it
// drives the frame's grey levels from paper, so residue left on the glass by a
// previous grey paint is added to the next one instead of being overwritten.
// Reader page turns get away with it because ReaderUtils::displayWithRefreshCycle
// forces a from-white FULL every `refreshFrequency` pages; a menu, a settings
// list or any other activity that repaints AA text has NO such counter, so it
// accumulates residue with nothing to discharge it.
//
// WHAT THIS IS. B1: the vendor's own policy, extended to UI grey paints. The
// vendor manages ghosting with a refresh CADENCE plus a user control, not by
// dithering text (RickyOS.bin strings: "Clean every %d", "Fewer pages between
// clean refreshes reduces ghosting, but flashes more often"), and this tree
// already implements the same cadence for pages. This adds the missing counter:
// EVERY N paints that actually take the grey lane, the NEXT one is driven as the
// existing from-white GC16 clean (the same drive `cadenceDue` already uses at
// :902 -- not a second waveform mechanism, no new LUT), and both counters reset.
//
// N is a compile-time constant, emitted by
// idf/components/crosspoint_app/CMakeLists.txt (and forwarded to this component's
// target there). The #ifndef below is a FALLBACK ONLY, exactly like the shipping
// CP_CUT_* values in src/display/RefreshPolicy.h: a real build defines it and the
// value in compile_commands.json / the configure log is what shipped.
//
// 0 disables the cadence (r155 behaviour at this site, one -D away).
// ---------------------------------------------------------------------------
#ifndef CP_UI_GREY_CLEAN_EVERY
#define CP_UI_GREY_CLEAN_EVERY 5  // fallback only: the shipping build defines this as 5
#endif
#include "freertos/task.h"

// App-side log sink (defined in src/main.cpp as LOG_INF("PANEL", ...)). Used so a drive's waveform,
// rail verdict, error code and duration reach debug.log, not just the power-off journal.
extern "C" void cp_panel_note(const char* line);

extern "C" {
#include "epdiy.h"
#include "config.h"
#include "epd_display.h"
#include "epd_highlevel.h"
#include "epd_board_metalio_eink4_plus.h"
#include "my_waveform.h"
#include "bus/metalio_i2c1.h"
#include "bus/metalio_i2c_ng.h"
#include "bus/tps65185.h"
}


// Board peripheral primitives (metalio_periph.c).
extern "C" {
esp_err_t mn_imu_init(void);
esp_err_t mn_imu_read(int16_t* x, int16_t* y, int16_t* z);
esp_err_t mn_rtc_read_time(int* y, int* mo, int* d, int* h, int* mi, int* s);
esp_err_t mn_gauge_read(int* mv, int* ma, int* pct);
esp_err_t mn_charger_read(uint8_t* status1);
int mn_charger_status(void);
const char* mn_charger_status_str(int st);
void mn_haptic_pulse(int ms);
void mn_audio_power(bool on);
esp_err_t mn_audio_init(void);
void mn_audio_deinit(void);
bool mn_rtc_int_pending(void);
bool mn_imu_activity(void);
}

// RTC-backed boot log + in-band signals (defined at the bottom of this file).
extern "C" {
void mn_note(const char* fmt, ...);
void mn_note_dump(void);
void mn_stage_signal(int n);
void mn_stage_signal_slow(int n);
}

namespace {

EpdiyHighlevelState g_hl;
bool g_begun = false;
bool g_powered = false;

// VID1 — short-DU bench frame countdown. 0 = nothing armed, which is every boot
// and every paint the shipping image performs. `exp vid N` sets it; each selected
// paint drives one whole-panel 8-phase frame and decrements. RAM only, so a reboot
// clears it, and no other code path may set it.
int s_shortDuRemaining = 0;
int s_shortDuPlanned = 0;

// FLP2 — "the next show() carries binary content and must leave on the DU lane".
//
// Armed by the renderer for exactly the one paint that follows a frontlight-panel
// adjustment (GfxRenderer::forceBinaryDuOnce -> the weak
// metalio_vendor_panel_binary_du_frames(1)). Three decisions in show() read it:
//   useGray8     (:644) the 8-optical-level wipe table -- the panel's frame is
//                       the screen underneath plus a card, so a reader page
//                       behind the sheet put this lane on EVERY percent drag;
//   forceFromWhite (:687) the from-white GC16 that a >=5-level HALF/GC16 frame
//                       gets -- another whole-panel drive;
//   cadenceDue   (:721)  the 24-partial clean, which can land on a panel paint
//                       because the counter is ambient (the reader's earlier
//                       partial paints accumulated it).
// All three are whole-panel by construction, so a paint that is honestly 1-bit
// content must be able to decline them. Countdown is 1 frame, RAM only, and a
// boot that never opens the panel executes the pre-FLP2 path exactly.
int s_binaryDuRemaining = 0;

// r156 (B1) — grey-lane paint counter. Incremented once per show() that ACTUALLY takes the
// grey (8-level wipe) lane, so it counts what the glass received, not what a call site asked
// for; reset when the grey-cadence clean runs. RAM only: a reboot starts from a clean panel, so
// there is nothing to preserve. Deliberately NOT keyed on time (the vendor-parity
// `kCleanMinIntervalMs` gate above is): a burst of menu repaints inside one minute is exactly
// the case that accumulated residue with no clean.
static unsigned s_uiGreyPaintsSinceClean = 0;
static unsigned s_uiGreyCleans = 0;  // observability only: how many cadence cleans this boot ran


// Draw result of the last show(), so the renderer can tell a drive that ran from one
// that never reached the glass (RE_RICKYOS_PIPELINE.md rows 7 and 12: the latch is
// written only on success and the shadow is declared stale on failure). 0 means no
// failure was reported. Surface codes are private to this file:
//   0x40000000  show() refused before driving (not begun / bad geometry / no buffer)
//   otherwise   the enum EpdDrawError the drive returned, unchanged.
static uint32_t g_last_draw_err = 0;
static constexpr uint32_t kPanelRefused = 0x40000000u;

// LOG1: below this many ms a show() did not drive the glass.
//
// The vendor's high-level update is diff-based and returns immediately when the frame is
// unchanged (highlevel.c:228 -- `diff_area.height == 0`). What the stopwatch then measures is
// the diff scan, not a waveform, so the line reads as an impossibly fast drive. Measured on
// THIS unit: the no-op signature is 52 ms (GL16) beside real drives of 373 ms (DU) and 801 ms
// (GC16) -- a sub-100 ms line for a multi-hundred-ms mode is a skipped update, never a speed-up.
// That illusion is what produced the phantom "DU is 53 ms" figure, so the line now says so.
// LOGGING ONLY: this constant decides what the log says, never whether the panel drives.
static constexpr uint32_t kNoDriveMsFloor = 100;

extern "C" uint32_t metalio_vendor_panel_last_draw_error(void) { return g_last_draw_err; }

// Registered by the app (main.cpp). A plain function pointer keeps this component free of any
// dependency on the input layer.
static MetalioPollHook g_poll_hook = nullptr;

extern "C" void metalio_vendor_panel_set_poll_hook(MetalioPollHook hook) { g_poll_hook = hook; }

// Set by the app (main.cpp) to route one line per show() into the power-off journal. Same shape as
// the poll hook: a plain function pointer, so this component never learns about storage.
static MetalioJournalHook g_journal_hook = nullptr;

extern "C" void metalio_vendor_panel_set_journal_hook(MetalioJournalHook hook) { g_journal_hook = hook; }

// r129 (H23): see the contract in the header. A plain function pointer, same reason as the two
// above -- this component never learns about storage, it only says "a waveform is clocking now".
// No hook installed means drive_gate() is an if on a null pointer: nothing is scheduled, nothing is
// delayed. LOGGING/IO-ORDERING ONLY at the storage layer; it cannot change a waveform.
static MetalioDriveGateHook g_driveGateHook = nullptr;

extern "C" void metalio_vendor_panel_set_drive_gate_hook(MetalioDriveGateHook hook) {
  g_driveGateHook = hook;
}

static inline void drive_gate(int active) {
  if (g_driveGateHook != nullptr) g_driveGateHook(active);
}

// r129 (H24): the [DRIVE] line's inputs. The three below live in display/output_lcd and are already
// maintained by the frame loop; declared here because this component does not include that header.
// C linkage, read-only, valid after a stopped scan -- they cannot perturb the drive they report.
extern "C" {
void epd_lcd_get_stream_stats(int* max_eof_gap_us, int* max_eof_work_us);
int epd_lcd_stream_budget_us(void);
int epd_lcd_stream_lines(void);
int epd_lcd_stream_clocked(void);
// r129 (H24): the runtime facts the [DRIVE] line reports, defined in main.cpp (Arduino-side, where
// getCpuFrequencyMhz()/esp_pm are in scope). Read-only; pm and the two lock bits are 0 in this
// build by construction (esp_pm is compiled out), which is exactly the answer being recorded.
int cp_cpu_mhz(void);
int cp_pm_enabled(void);
int cp_wake_pm_lock_state(void);
}

extern "C" void metalio_vendor_panel_set_priority(int priority) { epd_renderer_set_feed_priority(priority); }

// 4 bpp destination geometry, filled in by epd_hl_init/epdiy.
int g_w = 0;
int g_h = 0;
int g_stride4 = 0;  // bytes per row in the RAW vendor framebuffer (raw_w / 2)
int g_raw_w = 0;    // raw scan frame width  (1216)
int g_raw_h = 0;    // raw scan frame height (684)

// The app works in the LOGICAL portrait frame (684x1216) while the panel scans a
// RAW landscape frame (1216x684). One mapping, used by both the frame blit and the
// alignment pattern, so what you see and what is measured can never disagree.
// Matches epdiy's _rotate(EPD_ROT_PORTRAIT): swap(x,y) then x = raw_w - 1 - x.
static inline void put4(uint8_t* fb, int stride, int raw_x, int raw_y, uint8_t g) {
  if (raw_x < 0 || raw_y < 0 || raw_x >= 1216 || raw_y >= 684) return;
  uint8_t* q = fb + (size_t)raw_y * stride + (raw_x >> 1);
  if (raw_x & 1) *q = (uint8_t)((*q & 0x0F) | ((g & 0x0F) << 4));
  else           *q = (uint8_t)((*q & 0xF0) | (g & 0x0F));
}
static inline void put_logical(uint8_t* fb, int stride, int x, int y, uint8_t g) {
  put4(fb, stride, 1216 - 1 - y, x, g);   // logical portrait -> raw landscape
}

// --- 1-bpp -> 4-bpp row expand, table driven ---------------------------------
//
// The frame blit used to call put4() once per pixel: 1216 x 684 = 831,744 calls,
// each with a bounds test and a read-modify-write, measured at roughly the cost
// of the DU drive it feeds. The conversion is a pure function of eight source
// bits, so it is precomputed once.
//
// EQUIVALENCE, byte for byte. put4() stores the nibble for an EVEN x in the low
// half of the byte and an ODD x in the high half. Source bit 7 is x=0 (MSB
// first). So for one source byte v covering x = 8k .. 8k+7, the destination word
// is nibble(v bit 7-i) placed at bit 4*i. s_expand_lut[v] is exactly that, and
// writing it as a little-endian uint32 lands byte j at address j, which is the
// same byte put4() would have written. The table is therefore the loop, not an
// approximation of it: no pixel changes value, and the A/B of this commit is
// invisible on the glass by construction.
static uint32_t s_expand_lut[256];
static bool s_expand_lut_ready = false;

static void expand_lut_init() {
  for (int v = 0; v < 256; ++v) {
    uint32_t out = 0;
    for (int i = 0; i < 8; ++i) {
      const uint8_t nib = ((v >> (7 - i)) & 1) ? 0x0F : 0x00;  // 1 = WHITE
      out |= static_cast<uint32_t>(nib) << (4 * i);
    }
    s_expand_lut[v] = out;
  }
  s_expand_lut_ready = true;
}

// dst must be 4-byte aligned and hold at least (w + 7) / 8 * 4 bytes; the caller
// guarantees it because stride4 is always a multiple of 4 and epdiy's front_fb is
// 64-byte aligned on this target (highlevel.c: fb_align).
static inline void expand_row(const uint8_t* src, uint8_t* dst, int w) {
  const int whole = w >> 3;  // complete source bytes -> whole 32-bit stores
  uint32_t* d = reinterpret_cast<uint32_t*>(dst);
  for (int i = 0; i < whole; ++i) d[i] = s_expand_lut[src[i]];
  // Tail when w is not a multiple of 8. Unreachable for 1216 (152 x 8), kept so
  // this helper stays correct if the panel geometry ever changes.
  for (int x = whole * 8; x < w; ++x) {
    const uint8_t nib = ((src[x >> 3] >> (7 - (x & 7))) & 1) ? 0x0F : 0x00;
    uint8_t* q = dst + (x >> 1);
    if (x & 1) *q = static_cast<uint8_t>((*q & 0x0F) | (nib << 4));
    else       *q = static_cast<uint8_t>((*q & 0xF0) | nib);
  }
}

// How many DISTINCT optical levels does this 4bpp canvas use, and does it carry
// any level between ink and paper at all? `mask` is the accumulator (bit n =
// level n seen), returned so the caller can carry it across rows; the caller
// derives both answers from it:
//   frameHasGrey  = any bit set other than 0x0 (ink) and 0xF (paper) -- the
//                   question the FAST/AA grey-table switch asks;
//   frameLevels   = popcount(mask) -- the question the DRIVE SELECTION asks,
//                   because it separates the two things that both look like
//                   "grey" here:
//                     anti-aliased TEXT      -> 3-5 levels (ink, paper, a couple
//                                               of AA steps);
//                     16-LEVEL IMAGE CONTENT -> a real ramp, well past 5 (cover
//                                               art, photos, the sleep card).
// A byte is "binary" when both of its pixels are 0x0/0xF, i.e. the byte is one
// of 0x00 (ink,ink), 0xFF (paper,paper), 0x0F (paper,ink) or 0xF0 (ink,paper).
//
// Byte-wise on purpose: no alignment assumption on the caller's row pointer, and
// it early-exits once all 16 levels are known, so a real image costs a fraction
// of the frame. A fully binary frame (a menu list, an image page) pays the whole
// 608 B scan, which is ~0.4 M compares for the frame — single-digit milliseconds,
// against the 163-273 ms expand this path deletes.
static inline uint16_t row_level_mask(const uint8_t* row, int bytes, uint16_t mask) {
  for (int i = 0; i < bytes && mask != 0xFFFFu; ++i) {
    const uint8_t v = row[i];
    mask |= static_cast<uint16_t>(1u << (v >> 4));
    mask |= static_cast<uint16_t>(1u << (v & 0x0F));
  }
  return mask;
}

// G16 RAMP PROVENANCE. The question this answers, once and for all, is the one
// that the ghosting investigation kept guessing at: is the 16-level frame the
// panel drives a FULL, from-scratch build of the current canvas, or is it a
// merge that leaves last frame's pixels in place?
//
// It is a full replace, at every stage, and this is the first of them: show()
// copies EVERY row of the caller's canvas into epdiy's front_fb (the loop in
// the 4bpp branch), then epd_hl_update_screen_from_white() rebuilds epdiy's
// entire 1ppB difference image from front_fb/back_fb for the whole screen
// (highlevel.c:167 memset back_fb 0xFF, render.c:622-628 re-interlaces every
// row). No stage keeps a previous frame's pixel, so "the ramp is fed a stale or
// incremental source" cannot be true of this code path.
//
// That is a claim about the code, and the operator needs one about the glass.
// So on every 16-level frame we fingerprint the SOURCE we are about to drive:
// how much of it carries content, and a 32-bit hash. Two paints of the same art
// must print the same fnv= and nonpaper=; a ghost still visible under an
// identical frame is therefore ON THE GLASS, produced by the drive, and no
// amount of buffer rebuilding will move it. A changed fnv= instead says the
// content itself differs, which is the other possible answer and the one an
// incremental builder would produce.
//
// Cost: one linear pass over an already-copied frame, 16-level paints only
// (frameLevels >= 5). It is not in the text/AA path and not in the per-pixel
// loop.
struct G16FrameStats {
  uint32_t ink = 0;       // nibbles exactly 0x0 -- the darkest level
  uint32_t nonPaper = 0;  // nibbles that carry anything at all (not 0xF)
  uint32_t fnv = 2166136261u;
};

static G16FrameStats scan_g16_frame(const uint8_t* bw, int stride, int h) {
  G16FrameStats s;
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = bw + static_cast<size_t>(y) * stride;
    for (int i = 0; i < stride; ++i) {
      const uint8_t b = row[i];
      const uint8_t lo = b & 0x0F;
      const uint8_t hi = static_cast<uint8_t>(b >> 4);
      s.ink += (lo == 0x00) + (hi == 0x00);
      s.nonPaper += (lo != 0x0F) + (hi != 0x0F);
      s.fnv = (s.fnv ^ b) * 16777619u;
    }
  }
  return s;
}

// One-shot: the caller has promised that the FIRST paint after this boot is a
// whole-panel forced drive (CP_RM_FULL, epd_hl_update_screen_full, which clears
// MODE_SKIP_UNCHANGED and therefore conditions every pixel), so the boot clear
// below has nothing to add except ~810 ms of white over a frame the previous
// session deliberately left on the glass. Armed by main.cpp for a SILENT reboot.
bool g_keep_glass_on_boot = false;

}  // namespace

extern "C" void metalio_vendor_panel_keep_glass_on_boot(void) { g_keep_glass_on_boot = true; }

extern "C" bool metalio_vendor_panel_begin(void) {
  if (g_begun) return true;

  // Claim the I2C bus as Arduino's, first. esp32-hal-i2c-ng can create a bus but
  // cannot adopt one, so if vendor code went first every later Wire call would
  // return ESP_ERR_INVALID_STATE (259) forever.
  const bool wire_ok = Wire.begin(0, 1, 100000);
  // The BQ27220 stretches SCL (the vendor used I2C_LL_MAX_TIMEOUT for exactly
  // this); Arduino Wire's default timeout is too tight for multi-byte reads.
  Wire.setTimeOut(50);
  printf("[panel] Wire.begin(0,1,100k) -> %s\n",
         wire_ok ? "OK (Arduino owns the bus)" : "FAILED (someone else owns it)");
  fflush(stdout);


#ifdef CP_PERIPH_PROBE
  // PERIPHERAL BRING-UP PROBE (runs BEFORE epd_init, so the console is alive).
  // Aims to exercise every chip the hardware audit identified, on the one bus.
  // ---------------------------------------------------------------------
  {
    uint8_t chg = 0;
    if (mn_charger_read(&chg) == ESP_OK) {
      printf("[periph:pre] charger CX25601N 0x6B status1=0x%02X chg=%s\n",
             chg, mn_charger_status_str(mn_charger_status()));
    } else {
      printf("[periph:pre] charger 0x6B: no response\n");
    }

    if (mn_imu_init() == ESP_OK) {
      int16_t ax = 0, ay = 0, az = 0;
      if (mn_imu_read(&ax, &ay, &az) == ESP_OK) {
        printf("[periph:pre] IMU SC7A20H 0x19 xyz=%d,%d,%d\n", ax, ay, az);
      }
    } else {
      printf("[periph:pre] IMU 0x19: no response\n");
    }

    int mv = 0, ma = 0, pct = 0;
    if (mn_gauge_read(&mv, &ma, &pct) == ESP_OK) {
      printf("[periph:pre] gauge BQ27220 0x55 %d mV, %d mA, %d%%\n", mv, ma, pct);
    } else {
      printf("[periph:pre] gauge 0x55: no response\n");
    }

    int ry, rmo, rd, rh, rmi, rs;
    if (mn_rtc_read_time(&ry, &rmo, &rd, &rh, &rmi, &rs) == ESP_OK) {
      printf("[periph:pre] RTC PCF8563 0x51 %04d-%02d-%02d %02d:%02d:%02d\n",
             ry, rmo, rd, rh, rmi, rs);
    } else {
      printf("[periph:pre] RTC 0x51: no response\n");
    }

    printf("[periph:pre] RTC INT (TCA P1.6) %s\n",
           mn_rtc_int_pending() ? "PENDING" : "idle");
    printf("[periph:pre] IMU INT1 (GPIO2 wired-OR, AOI1_SRC) %s\n",
           mn_imu_activity() ? "ACTIVE" : "quiet");

#ifdef CP_AUDIO_PROBE
    // Brings up the BH1098G over UART1 (TX39/RX38) with the vendor AT
    // sequence. Off by default: it resets the audio module and costs ~1 s.
    if (mn_audio_init() == ESP_OK) {
      printf("[periph:pre] audio BH1098G ready (RX=2, MODE=1)\n");
    } else {
      printf("[periph:pre] audio BH1098G: no ACK\n");
    }
#endif
#ifdef CP_HAPTIC_PROBE
    // Boot self-test: one 50 ms pulse on the TCA P0.3 motor. Audible/tactile
    // proof that the haptic path exists in this image (it is otherwise
    // dead-stripped when the app has no caller).
    mn_haptic_pulse(50);
    printf("[periph:pre] haptic TCA P0.3: 50 ms pulse\n");
#endif
    fflush(stdout);
  }
#endif  // CP_PERIPH_PROBE

  mn_note("[rtc] calling epd_init\n");
  // r97 Phase 2 — EPD_LUT_64K A/B (METALIO_EPD_LUT_64K, CMake cache var, default 0).
  //   idf.py -B ../build-lut64k -DMETALIO_EPD_LUT_64K=ON build
  // THIS is the live epd_init(): MetalioEpd_Init() in display/metalio_epd.c has no
  // caller and is dead-stripped, so the switch started there and changed nothing.
  // 1K is what this fork has always shipped and a plain build keeps it.
  // It differs only for the 2 bpp modes (GC16/GL16 page turns): find_lut_functions()
  // picks build_2ppB_lut_1k + calc_epd_input_2ppB_1k_lut_{white,black} at 1K and
  // build_2ppB_lut_64k_* + calc_epd_input_2ppB_lut_64k at 64K — different algorithms,
  // not a bigger table for the same one. MODE_PACKING_1PPB_DIFFERENCE takes the 1K
  // vector pair on RENDER_METHOD_LCD whatever the allocation is (render.c says so),
  // so a DU/partial A/B would measure nothing. The extra 63 KB comes from INTERNAL
  // RAM (render.c: MALLOC_CAP_INTERNAL), not PSRAM.
#if METALIO_EPD_LUT_64K
  const enum EpdInitOptions lutOption = EPD_LUT_64K;
#else
  const enum EpdInitOptions lutOption = EPD_LUT_1K;
#endif
  // Printed BEFORE epd_init(): driving GPIO19 (EPD_PIN_MODE, shared with USB D-)
  // kills the console, so this is the last point where the console can say which
  // LUT format the running image uses.
  printf("[panel] epd_init LUT=%s (METALIO_EPD_LUT_64K=%d)\n", METALIO_EPD_LUT_64K ? "64K" : "1K",
         (int)(METALIO_EPD_LUT_64K));
  fflush(stdout);
  epd_init(&epd_board_metalio_eink4_plus, &ED047TC2_1216, lutOption);
  const bool ioexp_ok = epd_board_metalio_eink4_plus_ioexp_ok();
  mn_note("[rtc] epd_init returned; ioexp_ok=%d\n", (int)ioexp_ok);

#ifdef CP_PERIPH_PROBE
  // AUTHORITATIVE peripheral pass. The block above runs before epd_init(), i.e.
  // before the TCA9555 is configured and before the rails are driven — chips fed
  // by P0.5 (camera/screen power) can legitimately look absent there. This pass
  // runs with the expander up, and records into the RTC-retained note buffer
  // because the console (GPIO19 = USB D-) is no longer readable after display
  // init. Read it back with mn_note_dump()/the on-panel diagnostics.
  // -------------------------------------------------------------------------
  {
    int mv = 0, ma = 0, pct = 0;
    int gy = 0, gmo = 0, gd = 0, gh = 0, gmi = 0, gs = 0;
    int16_t ax = 0, ay = 0, az = 0;
    uint8_t chg = 0;

    const bool chg_ok = (mn_charger_read(&chg) == ESP_OK);
    const bool imu_ok = (mn_imu_init() == ESP_OK) &&
                        (mn_imu_read(&ax, &ay, &az) == ESP_OK);
    const bool gau_ok = (mn_gauge_read(&mv, &ma, &pct) == ESP_OK);
    const bool rtc_ok = (mn_rtc_read_time(&gy, &gmo, &gd, &gh, &gmi, &gs) == ESP_OK);

    mn_note("[periph:post] ioexp=%d chg=%s vbus_chg=%s imu=%s%d,%d,%d "
            "gauge=%s%d mV %d mA %d%% rtc=%s%04d-%02d-%02d %02d:%02d:%02d "
            "rtc_int=%d imu_int=%d\n",
            (int)ioexp_ok,
            chg_ok ? "ok" : "ABSENT", mn_charger_status_str(mn_charger_status()),
            imu_ok ? "ok " : "ABSENT ", ax, ay, az,
            gau_ok ? "ok " : "ABSENT ", mv, ma, pct,
            rtc_ok ? "ok " : "ABSENT ", gy, gmo, gd, gh, gmi, gs,
            (int)mn_rtc_int_pending(), (int)mn_imu_activity());
  }
#endif  // CP_PERIPH_PROBE

  // In-band stage signal: 1 pulse = expander probe OK, 3 SLOW pulses = failed.
  // Bounded, never an infinite strobe: that would hang setup() and hide every
  // later diagnostic.
  if (ioexp_ok) {
    mn_stage_signal(1);
  } else {
    mn_stage_signal_slow(3);
  }

  // r163 D9: the stored black-depth value (1200 unless a setting stored another one before
  // this call). One copy of the number: metalio_vendor_panel_vcom_mv(), not a literal here.
  epd_set_vcom(static_cast<uint16_t>(metalio_vendor_panel_vcom_mv()));
  epd_set_rotation(EPD_ROT_LANDSCAPE);
  my_waveform_init();
  g_hl = epd_hl_init(&MY_WAVEFORM);

  g_raw_w = epd_width();          // raw scan frame: 1216 x 684 (landscape)
  g_raw_h = epd_height();
  g_stride4 = g_raw_w / 2;        // bytes per RAW row in the vendor framebuffer
  // What CrossPoint is told: the vendor's own LOGICAL/rotated size, 684 x 1216.
  // Reporting the raw frame here is what made the app render landscape while the
  // touch driver mapped portrait, so both touch axes looked swapped and inverted.
  // See epd_rotated_display_width()/height() in epdiy.c.
  g_w = epd_rotated_display_width();
  g_h = epd_rotated_display_height();

  printf("[panel] vendor epdiy ready %dx%d (stride4=%d, spiram ok=%d)\n", g_w, g_h, g_stride4,
         g_hl.front_fb != nullptr);
  fflush(stdout);

  if (!g_hl.front_fb || g_w <= 0 || g_h <= 0) return false;

  g_begun = true;

  // Physically whiten the glass once: without it the first diff-based update has
  // no trustworthy baseline.
  mn_note("[rtc] calling epd_poweron\n");
  epd_poweron();
  mn_note("[rtc] epd_poweron returned\n");
  mn_stage_signal(2);
  g_powered = true;

  // THE BOOT CLEAR, and the one case it must not run.
  //
  // Its job is the SOFTWARE baseline, and that is set by epd_hl_init() above
  // (memset front_fb = back_fb = 0xFF, "assume white"), not by the glass. What the
  // clear adds is a physical white drive: on an ordinary boot nothing is on the
  // glass yet, so it costs nothing and settles the rails.
  //
  // On a SILENT reboot (heap-defrag ESP.restart()) the glass is carrying a frame
  // THIS firmware just drove -- silentRestartTo() draws the "Loading…" popup 50 ms
  // before the reset, and e-paper holds it across the restart. Whitening the glass
  // here erases that popup and leaves ~810 ms of nothing before the arriving
  // screen's own paint, which is the blank/white frame the operator reports after a
  // KOReader sync return and after an NTP clock sync (both exit through a silent
  // reboot). main.cpp arms keep_glass_on_boot ONLY when the first paint after this
  // boot is a guaranteed whole-panel forced clean (it arms forceCleanNextRefresh()
  // for that whole branch), so every pixel is re-driven from white afterwards and
  // the clear cannot be the difference between correct and ghosted. See
  // main.cpp, "THE SILENT-REBOOT GLASS CONTRACT".
  if (g_keep_glass_on_boot) {
    mn_note("[rtc] boot clear skipped (keep-glass: glass holds the exit frame)\n");
    if (g_journal_hook) g_journal_hook("[panel] boot clear skipped: silent reboot keeps the exit frame");
  } else {
    mn_note("[rtc] calling epd_clear\n");
    epd_clear();
    mn_note("[rtc] epd_clear returned\n");
  }
  g_keep_glass_on_boot = false;  // one-shot: a later begin() in this boot must not skip it

#ifdef CP_BOOT_SPLASH
  // PRODUCT BOOT SPLASH. One GC16 push, no hold: proves the panel is alive within
  // ~2 s of power-on and gives the app's own init a visible background. Geometry
  // is a frame plus a progress rule drawn in LOGICAL portrait coordinates through
  // the same mapping the UI blit uses.
  {
    uint8_t* fb = g_hl.front_fb;
    const int W = g_w, H = g_h, S = g_stride4;
    auto rect = [&](int x0, int y0, int x1, int y1, uint8_t g) {
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) put4(fb, S, x, y, g);
    };
    rect(0, 0, W - 1, H - 1, 0x0F);                 // paper white
    rect(48, H / 2 - 60, W - 49, H / 2 - 58, 0x00);  // upper rule
    rect(48, H / 2 + 58, W - 49, H / 2 + 60, 0x00);  // lower rule
    rect(W / 2 - 90, H / 2 - 6, W / 2 + 90, H / 2 + 6, 0x00);  // centre block: mark
    epd_hl_update_screen(&g_hl, MODE_GC16, 25);
    printf("[panel] boot splash pushed %dx%d\n", W, H);
    fflush(stdout);
  }
#endif

  epd_poweroff();
  g_powered = false;
  return true;
}

extern "C" void metalio_vendor_panel_show(const uint8_t* bw, int w, int h, int stride,
                                          int refresh, bool power_off_after) {
  if (!g_begun || !bw || w <= 0 || h <= 0) {
      g_last_draw_err = kPanelRefused;
      return;
    }
    if (w != g_w || h != g_h) {
      // Geometry mismatch means the facade and the panel disagree; refusing is
      // safer than writing a wrongly-sized frame into the vendor buffer.
      printf("[panel] REFUSING show: facade %dx%d != panel %dx%d\n", w, h, g_w, g_h);
      fflush(stdout);
      g_last_draw_err = kPanelRefused;
      return;
    }

    uint8_t* dst = g_hl.front_fb;
    if (!dst) {
      g_last_draw_err = kPanelRefused;
      return;
    }

  mn_note("[rtc] show ENTRY t=%lu ms\n", (unsigned long)(millis()));

  // FRAME TRANSFER. Two source formats, told apart by the stride the driver
  // passed (it is the canvas row width in BYTES, so the format is unambiguous):
  //   stride == w/2 : 4 bpp canvas (CP_RENDER_4BPP). Same nibble layout as
  //                   front_fb — even x in the low nibble — so the whole frame
  //                   is a row copy, and the 1 bpp -> 4 bpp expand (measured
  //                   163-273 ms, i.e. the largest single CPU term of a page
  //                   turn) ceases to exist.
  //   stride == w/8 : 1 bpp canvas. Expanded through the 256-entry LUT, as
  //                   before; this path is what -DCP_RENDER_4BPP=OFF builds.
  //
  // It also answers the question the waveform selection needs: does this frame
  // actually carry more than two optical levels? A frame whose nibbles are all
  // 0x0/0xF renders identically on the binary DU table, so it must not pay the
  // grey table's extra phases.
  const bool src4bpp = (stride * 2 == w);
  bool frameHasGrey = false;
  uint16_t frameLevelMask = 0;
  if (src4bpp) {
    if (stride != g_stride4) {
      printf("[panel] REFUSING show: 4bpp stride %d != panel %d\n", stride, g_stride4);
      fflush(stdout);
      g_last_draw_err = kPanelRefused;
      return;
    }
    for (int y = 0; y < h; ++y) {
      const uint8_t* srow = bw + (size_t)y * stride;
      memcpy(dst + (size_t)y * g_stride4, srow, (size_t)g_stride4);
      // One scan per row for BOTH answers: the boolean the AA/FAST path needs and
      // the level count the drive-selection needs. Early-exits once all 16 levels
      // are known, so a real image costs a fraction of the frame.
      frameLevelMask = row_level_mask(srow, g_stride4, frameLevelMask);
    }
    frameHasGrey = (frameLevelMask & ~((1u << 0) | (1u << 15))) != 0;
  } else {
    // 1-bpp source (bit set = WHITE) -> 4-bpp nibble. Rows map 1:1: the app hands
    // us a 1216x684 LANDSCAPE frame and vendor _rotate() for EPD_ROT_LANDSCAPE is
    // the identity, so source row y is destination raw row y and source byte i is
    // destination word i. That is why this is a row-at-a-time table expand and not
    // a per-pixel put4(): same nibbles, ~25x fewer operations.
    if (!s_expand_lut_ready) expand_lut_init();
    for (int y = 0; y < h; ++y) {
      expand_row(bw + (size_t)y * stride, dst + (size_t)y * g_stride4, w);
    }
  }

  // Rail truth immediately before the drive, read LIVE from the expander. tps_ok() is the cached
  // verdict from the last poweron and happily reads 1 with the rails down, which is exactly how a
  // genuine 48-phase drive can clock with nothing on the glass. PWR_GOOD (TCA P07) is the panel's
  // own answer, so cycle the rails only when it reads low; a healthy rail keeps its gap-free path.
  constexpr int kPgoodPort = EPD_TPS_IO_PWR_GOOD >> 3;
  constexpr uint8_t kPgoodBit = 1u << (EPD_TPS_IO_PWR_GOOD & 7);
  uint8_t pgPort = 0xFF;
  // A dead expander is a different fault: assume up rather than power-cycling on a read that never
  // happened.
  const bool pgKnown = epd_board_metalio_eink4_plus_ioexp_read_port(kPgoodPort, &pgPort);
  const bool pgoodHigh = !pgKnown || (pgPort & kPgoodBit) != 0;
  if (!pgoodHigh) metalio_vendor_panel_repower();
  if (g_journal_hook) {
    char pgLine[96];
    snprintf(pgLine, sizeof(pgLine), "[panel] pg=%d %s", pgoodHigh ? 1 : 0, pgoodHigh ? "no cycle" : "cycled");
    g_journal_hook(pgLine);
  }

  if (!g_powered) {
    epd_poweron();
    // r85: this gate has to follow the rail verdict, not the call. epd_poweron()
    // no longer reboots on failure -- it latches s_tps_ok = false (see the FAILURE
    // CONTRACT in epd_board_metalio_eink4_plus.c) -- so an unconditional
    // g_powered = true here would make the NEXT paint skip the power-up and drive
    // a dead panel forever. Following tps_ok() keeps latch-and-retry: a failed
    // power-up leaves g_powered false and the next show() retries it.
    g_powered = epd_board_metalio_eink4_plus_tps_ok();
  }
  // WAVEFORM SELECTION. This used to be `full ? MODE_GC16 : MODE_GL16`, which sent HALF_REFRESH
  // (the app's ghost-cleaning cadence, every 15 pages) and FAST_REFRESH (every page turn and
  // every UI update) to the SAME 48-phase grayscale waveform. Two consequences, both measured by
  // the operator: a page turn flashed for ~1 s with an inverted intermediate frame (GL16's
  // erase/saturate phases), and the periodic clean never actually ran, so ghosting accumulated.
  // MODE_DU is the binary waveform (20 phases) and is the right one for 1-bit content; GC16 is
  // the 48-phase table, kept for cleaning and for the sleep screen.
  const bool clean = (refresh != METALIO_PANEL_REFRESH_FAST);
  // FLP2: this show() is a paint the caller declared binary (see
  // s_binaryDuRemaining). Read once, so every selection below sees the same
  // answer, and spent once at the end of this call.
  const bool binaryDuForced = (s_binaryDuRemaining > 0);
  // GREY WAVEFORM SELECTION. The frame we just copied tells us which content
  // class this is:
  //   grey present + FAST  -> the 8-optical-level grey ramp, reached through
  //                           my_waveform_wipe_gl(3): 8 DU erase phases that
  //                           drive every pixel to paper, then the 30-phase
  //                           8-grey table looked up from source white only
  //                           (38 phases, ~456 ms). Reader AA text is the only
  //                           content that reaches here - see fillRectImpl and
  //                           the image writers, which keep geometry and bitmaps
  //                           at ink/paper.
  //   binary + FAST        -> DU (20 phases, ~240 ms). Ordinary page turns, menu
  //                           repaints and image pages never pay the grey table.
  //   HALF / GL16 / FULL   -> unchanged: GC16, GL16, GC16-from-white.
  // The swap is per-drive through epd_hl_waveform() and is restored immediately
  // after, so no other path can inherit a table that only exposes .type = 5
  // (a MODE_DU/MODE_GC16 request against it would find no mode at all).
  const bool useGray8 = !clean && src4bpp && frameHasGrey && !binaryDuForced;
  // 16-LEVEL FRAMES NEVER GO DOWN AS A DIFF (r101.3). This is the DRIVE-SELECTION
  // bug behind the GRAY16 ghosting, and it is a trap that sits between two layers
  // that each look correct on their own:
  //
  //   * The call sites ask for a CLEAN: the sleep screen requests FULL
  //     (SleepActivity.cpp sleepDriveMode -> HalDisplay::FULL_REFRESH) and a
  //     toned image page requests FULL too (EpubReaderActivity imageDrive).
  //   * The renderer's producer stage can legitimately replace that request:
  //     cp_refresh_resolve_request() (RefreshPolicy.h:347) returns the armed
  //     AUTO/promotion verbatim, and an AUTO that measures few changed pixels
  //     resolves to FAST or HALF -- by design, "AUTO must never escalate to a
  //     whole-panel full drive". So the panel is handed HALF (1), not FULL (0),
  //     for a frame that is a whole 16-level IMAGE.
  //   * `clean` below is only `refresh != FAST`, so HALF counts as "clean" and
  //     selects the 48-phase GC16 table -- but the drive at the bottom of this
  //     function is epd_hl_update_screen(MODE_GC16), i.e. the DIFFING entry
  //     point. Only refresh==FULL (:627) and the cadence arm (:650) reach
  //     epd_hl_update_screen_from_white(). epd_hl_update_screen() sets
  //     MODE_SKIP_UNCHANGED, so every pixel whose target equals the recorded
  //     baseline is never pulsed and the previous page's ink under the cover art
  //     survives -- exactly the rows-inside-the-picture and the offset duplicate
  //     on the sleep card that the operator photographed. The waveform is not
  //     wrong here; the ENTRY POINT is, and a "clean" request that skips
  //     unchanged pixels is not a clean at all.
  //
  // The panel is the last layer that knows BOTH facts -- the requested class and
  // whether the frame actually carries more than two optical levels -- so it
  // must not delegate the from-white decision to an upstream request that may
  // already have been demoted. A frame that carries a real 16-level ramp
  // (>= kSixteenLevelFrameLevels distinct optical levels, not the 3-5 of AA text)
  // and is being driven by anything in the GC16 family goes from white, which
  // resets back_fb to 0xFF (highlevel.c: epd_hl_update_screen_from_white) so
  // EVERY pixel is driven and the 15->15 conditioning phase actually runs.
  //
  // Deliberately NOT covered: FAST (grey AA text is the 8-level wipe table's job,
  // useGray8 above), GL16 and EQUALIZE (their own drive contracts elsewhere), and
  // anything under 5 optical levels -- which is where a 2-bit grayscale BASE and
  // anti-aliased text live, and both must keep whatever waveform their own
  // contract names (the sleep nudge LUT is calibrated against a plain HALF base,
  // see SleepActivity.cpp:867-871).
  constexpr int kSixteenLevelFrameLevels = 5;
  const int frameLevels = __builtin_popcount(static_cast<unsigned>(frameLevelMask));
  const bool grey16Frame = src4bpp && frameLevels >= kSixteenLevelFrameLevels;
  const bool gc16Class = (refresh == METALIO_PANEL_REFRESH_FULL || refresh == METALIO_PANEL_REFRESH_HALF);
  const bool forceFromWhite = grey16Frame && gc16Class && !binaryDuForced;
  // The on-glass proof of the full-replace claim above: one line per 16-level
  // frame, from the bytes we are ABOUT to hand epdiy, so it cannot lie about
  // what the panel was given. grep `[G16]` -- `full=1` means every row of the
  // frame came from the live canvas in this call, and `fnv=` is the frame's
  // fingerprint: identical art must repeat it, and a ghost still visible under a
  // repeated hash is a drive residual, not a buffer leak.
  if (grey16Frame) {
    const G16FrameStats st = scan_g16_frame(bw, g_stride4, h);
    char g16line[192];
    snprintf(g16line, sizeof(g16line),
             "[G16] ramp rows=%d/%d src=canvas full=1 levels=%d ink=%lu nonpaper=%lu fnv=%08lx", h, g_h,
             frameLevels, static_cast<unsigned long>(st.ink), static_cast<unsigned long>(st.nonPaper),
             static_cast<unsigned long>(st.fnv));
    cp_panel_note(g16line);
    if (g_journal_hook) g_journal_hook(g16line);
  }
  // RE-CONDITION CADENCE (vendor parity, epd_refresh.cc:450). The OEM worker
  // promotes a partial drive to a FULL GC16 clean when it has run 24 partials
  // AND at least 60 s have passed since the last full one:
  //   promote = ... changed>0 && partials.load()>=24
  //                       && esp_timer_get_time()-last_full>=60000000
  // and resets both counters only when the GC16 actually ran (mode==MODE_GC16).
  // Without a cadence the residual of a grey drive has nowhere to go:
  // epd_hl_update_screen() drives ONLY changed pixels (MODE_SKIP_UNCHANGED zeroes
  // the from==to diagonal in changed_only_phase(), lut.c), so a pixel whose
  // target equals its recorded value is never pulsed -- and the app's own
  // periodic "clean" is a no-op for exactly those pixels. This bound is the
  // OEM's answer, and it caps any drift at 24 turns.
  constexpr unsigned kCleanEveryPartials = 24;
  constexpr uint32_t kCleanMinIntervalMs = 60000;
  static unsigned s_partialsSinceClean = 0;
  static uint32_t s_lastCleanMs = 0;
  const bool cadenceDue = !clean && !binaryDuForced && s_partialsSinceClean >= kCleanEveryPartials &&
                          (s_lastCleanMs == 0 || (uint32_t)(millis() - s_lastCleanMs) >= kCleanMinIntervalMs);
  // r156 (B1) — UI GREY-PAINT CADENCE (the AA-ghosting fix; see CP_UI_GREY_CLEAN_EVERY at the
  // top of this file). This is the counter the reader has and a menu does not:
  //   * it counts paints that will ACTUALLY take the grey lane (`greyLaneWanted`), so a paint
  //     already promoted to the 24-partial clean, or declined by the forced-binary arm, does not
  //     consume a slot;
  //   * when the Nth one arrives, THAT paint is driven as a from-white GC16 clean instead of the
  //     grey ramp -- the same `epd_hl_update_screen_from_white(MODE_GC16)` drive `cadenceDue`
  //     uses, armed by a count instead of by 24 partials + a 60 s gate. No new waveform and no
  //     new LUT: the frame it lands on is the current frame, so the user sees the same screen,
  //     cleaned, not a blank flash.
  //   * time is deliberately NOT a term. The vendor-parity gate above needs 60 s since the last
  //     clean, which is exactly why a fast run of menu repaints could accumulate residue with no
  //     clean available to it. Residue accrues per paint, so the cadence is per paint.
  constexpr unsigned kUiGreyCleanEvery =
      (CP_UI_GREY_CLEAN_EVERY) > 0 ? static_cast<unsigned>(CP_UI_GREY_CLEAN_EVERY) : 0u;
  const bool greyLaneWanted = useGray8 && !cadenceDue;
  const bool greyCadenceDue =
      greyLaneWanted && kUiGreyCleanEvery != 0 && (s_uiGreyPaintsSinceClean + 1) >= kUiGreyCleanEvery;
  if (greyLaneWanted && !greyCadenceDue) ++s_uiGreyPaintsSinceClean;
  // ... and the table swap is only taken when the wipe table actually built: on
  // allocation failure the wiper returns NULL, and handing NULL to
  // epd_hl_waveform() would leave the mode lookup with no table at all. Falling
  // back to DU for that one frame is a thresholded page, not a dead panel.
  const EpdWaveform* const greyWf = (greyLaneWanted && !greyCadenceDue) ? my_waveform_wipe_gl(3) : nullptr;
  const bool greyDrive = greyWf != nullptr;
  // VID1 — the 8-phase table that is built at init (my_waveform.c short_du_build) and
  // otherwise never selected. `exp vid N` arms it; this predicate is false on every
  // paint of a boot that never typed the command, so the shipped lane is untouched.
  // Only a FAST (binary) request may take it: the table has one mode (type 1 = DU).
  const EpdWaveform* const vidWf = (s_shortDuRemaining > 0 && !clean) ? my_waveform_short_du() : nullptr;
  const bool vidDrive = vidWf != nullptr;
  const char* modeName = forceFromWhite                          ? "GC16-from-white"
                         : refresh == METALIO_PANEL_REFRESH_FULL  ? "GC16-from-state"
                         : refresh == METALIO_PANEL_REFRESH_GL16  ? "GL16"
                         : refresh == METALIO_PANEL_REFRESH_HALF  ? "GC16"
                         : refresh == METALIO_PANEL_REFRESH_EQUALIZE ? "EQUALIZE_BLACK"
                         : cadenceDue                            ? "GC16(clean-cadence)"
                         : greyCadenceDue                        ? "GC16(ui-grey-cadence)"
                         : vidDrive                              ? "shortDU(8)"
                         : greyDrive                             ? "gray8-wipe-GL16(38)"
                                                                 : "DU";
  // The grey table is only correct as an ERASE-THEN-GREY drive. Its (from,to)
  // LUT is 30 phases of an 8-level optical ramp with no saturating erase of its
  // own: driven differentially it cannot restore an endpoint, and every page
  // turn leaves a little more residual - the "contrast decays with each flip"
  // signature. The OEM never drives it that way: for reader AA it builds
  // my_waveform_wipe_gl(), which prefixes DU drive-to-white frames (every `old`
  // state mapped through the 15<-old column) and then looks the grey ramp up
  // ONLY against source white. The panel is therefore conditioned to paper
  // before the grey ramp starts, on every single turn.
  //
  // profile 3 = 8 erase phases + the 30-phase 8-grey table = 38 phases
  // (~456 ms at the 12 ms floor): the same table as a004f02c, driven the way the
  // vendor drives it, at +96 ms instead of the un-conditioned 30.
  if (greyDrive) epd_hl_waveform(&g_hl, greyWf);
  if (vidDrive) epd_hl_waveform(&g_hl, vidWf);
  // FLP2: this show() has now made every decision it is going to make, so the
  // forced-binary frame countdown is spent whatever the drive does next (a
  // diff-empty no-op still belonged to that paint).
  if (binaryDuForced) --s_binaryDuRemaining;
  mn_note("[rtc] show: update start (mode=%s req=%d levels=%d fromwhite=%d) t=%lu ms\n", modeName, refresh,
          frameLevels, forceFromWhite ? 1 : 0, (unsigned long)(millis()));
  const uint32_t tStart = millis();
  // r129 (H23): hold the SD log writer for the whole waveform -- every branch below, including the
  // 0x400 recovery. Released right after the drive returns, which drains the lines it held back.
  // No-op when the writer is already idle (logSetDriveActive(false) on an empty ring does nothing).
  drive_gate(1);
  // Entry edge: collect anything that arrived while the caller composed the frame.
  if (g_poll_hook) g_poll_hook();
  // The draw result is the only evidence that the waveform actually ran; it used to be dropped
  // on the floor, so a "48 phases clocked but the glass kept the old frame" report could not be
  // told apart from a genuine drive. Keep it and journal it.
  enum EpdDrawError drawErr = EPD_DRAW_SUCCESS;
  if (forceFromWhite || refresh == METALIO_PANEL_REFRESH_FULL) {
    // A manual "refresh screen" must actually refresh, AND so must any frame that
    // carries grey content (see "16-LEVEL FRAMES NEVER GO DOWN AS A DIFF" above):
    // a demoted request must not turn a whole 16-level image into a diff-area
    // drive. The diff-based update returns immediately when the frame is
    // unchanged (highlevel.c:228 `diff_area.height == 0`), and front_fb is
    // precisely the buffer our blit just wrote -- so refreshing an identical page
    // produced no waveform at all and nothing appeared to happen. Driving from an
    // assumed-white baseline makes the drive full-screen, which is what both
    // callers asked for: a visible flash and a real ghost clean.
    // WHEN the frame is a real 16-level ramp (forceFromWhite), the from-white drive
    // gets an ACTUAL erase sweep first -- see "ERASE SWEEP" below.
    //
    // ERASE SWEEP. `from_white` is only a claim: highlevel.c:167 memsets back_fb to
    // 0xFF and then looks every pixel up as 15 -> target, which is true only if the
    // glass is already paper. Over a previous page it is not, and the weak cells are
    // exactly the mid-grey targets: 15 -> 8 is a short white-to-light-grey move, so
    // the ink underneath the old glyphs is under-erased and survives as the faint
    // residue. A text page hides this structurally -- targets are 0, and 15 -> 0 is
    // the long sweep. So drive one real all-paper frame first, through the same
    // epd_hl_update_screen_full path the rest of the driver uses, with a table whose
    // every cell is the DU to-white column (my_waveform_erase_white(), the same erase
    // prefix my_waveform_wipe_gl() applies for the 8-grey path): every pixel is
    // pulsed to white whatever it currently holds. Afterwards front_fb/back_fb both
    // legitimately hold 0xFF, so the content drive below starts from a precondition
    // that is now TRUE rather than assumed.
    // Cost: 20 phases ~= 240 ms, only on 16-level GC16 paints (images, sleep card).
    // The erase frame is never shown as content: it and the content drive happen back
    // to back inside this call, so the user still sees one flash.
    enum EpdDrawError eraseErr = EPD_DRAW_SUCCESS;
    // r159 (option 1): did this call actually make the glass paper? from_white is only a
    // CLAIM (highlevel.c:167 memsets back_fb to 0xFF). It is true after the erase sweep
    // below ran and succeeded, and false otherwise -- and "otherwise" is every menu/UI
    // frame, which is not a 16-level ramp so forceFromWhite is false and the sweep is
    // skipped while from_white was still being used. That skip is the ghost: a pixel that
    // was dark in the previous screen and is paper in this one has from=15 -> to=15 under
    // the claim, so it is never driven and the old glyph survives on the glass.
    bool glassIsPaper = false;
    if (forceFromWhite) {
      const EpdWaveform* const eraseWf = my_waveform_erase_white();
      if (eraseWf) {
        memset(dst, 0xFF, (size_t)g_stride4 * h);  // target = paper, every pixel
        epd_hl_waveform(&g_hl, eraseWf);
        const uint32_t tErase = millis();
        eraseErr = epd_hl_update_screen_full(&g_hl, MODE_GC16, 25);
        const uint32_t eraseMs = millis() - tErase;
        glassIsPaper = (eraseErr == EPD_DRAW_SUCCESS);
        // Restore MY_WAVEFORM before anything else: the 0x400 retry path and every
        // later drive assume it is resident (it is the only table with DU/GC16).
        epd_hl_waveform(&g_hl, &MY_WAVEFORM);
        // Put the real frame back for the content drive.
        for (int y = 0; y < h; ++y) {
          memcpy(dst + (size_t)y * g_stride4, bw + (size_t)y * stride, (size_t)g_stride4);
        }
        char sweep[160];
        snprintf(sweep, sizeof(sweep),
                 "[G16] erase sweep ms=%lu rows=%d/%d to=paper phases=20 err=0x%x", (unsigned long)eraseMs,
                 h, g_h, (unsigned)eraseErr);
        cp_panel_note(sweep);
        if (g_journal_hook) g_journal_hook(sweep);
      }
    }
    // Take the from-white drive only when the glass really is paper. Otherwise drive the
    // FULL panel from the REAL previous frame: _full clears MODE_SKIP_UNCHANGED and diffs
    // front against back, so every pixel is driven with its true from-level -- the pixels
    // the false-white claim used to skip are now driven, which is the whole point.
    // Cost is unchanged (same table, same phase count); the drive was already full-panel.
    drawErr = glassIsPaper ? epd_hl_update_screen_from_white(&g_hl, MODE_GC16, 25)
                           : epd_hl_update_screen_full(&g_hl, MODE_GC16, 25);
    // A failed erase leaves the glass in an unknown state; surface it as this frame's
    // error so the 0x400 recovery below (checked clear + from-white retry) runs.
    drawErr = static_cast<EpdDrawError>(drawErr | eraseErr);
  } else if (refresh == METALIO_PANEL_REFRESH_GL16) {
    // Fourth mode: the vendor's 16-level grayscale table. 48 frames like GC16, but it drives the
    // 15->15 transition (white tick), so white areas are actively cleaned. Diff-based either way.
    drawErr = epd_hl_update_screen(&g_hl, MODE_GL16, 25);
  } else if (refresh == METALIO_PANEL_REFRESH_EQUALIZE) {
    // Fifth mode: QUICK REFRESH's wipe (vendor v2.0.74 MODE_EQUALIZE_BLACK, upstream 2b2c36a).
    // Same 48-frame GC16 table, but the LUT is rebuilt per phase so that every pixel whose
    // TARGET is black is driven black whatever it currently shows -- including pixels the diff
    // says are unchanged -- and highlevel.c drives the whole panel rather than the changed
    // rectangle. So this is a page-wide re-ink of the black level: faded or washed-out glyphs
    // come back solid instead of the previous diff-based GC16, which re-drove only the changed
    // rectangle at the same cost (measured 899 ms and indistinguishable from a page turn).
    // NOT an erase: pixels whose target is white keep their normal transition, so the frame on
    // the glass is still the page. Nothing needs to repaint afterwards and no black frame is
    // ever shown, which is why this needs no second drive and no renderer change.
    drawErr = epd_hl_update_screen(&g_hl, static_cast<EpdDrawMode>(MODE_GC16 | MODE_EQUALIZE_BLACK), 25);
  } else if (cadenceDue || greyCadenceDue) {
    // The bounded clean. from_white memsets back_fb, so EVERY pixel is driven
    // (from=15 for all), which is the only thing in this driver that actually
    // conditions unchanged pixels. 48-phase GC16: ~576 ms, once per 24 partials
    // at most. The counter resets only when the drive succeeded.
    //
    // r156 (B1): `greyCadenceDue` reaches this same drive, armed by the UI grey-paint count
    // (CP_UI_GREY_CLEAN_EVERY) rather than by the 24-partials/60 s vendor gate. Both counters
    // are discharged on success because this drive cleans every pixel, i.e. it is a superset
    // of what either counter was measuring.
    drawErr = epd_hl_update_screen_from_white(&g_hl, MODE_GC16, 25);
    if (!drawErr) {
      s_partialsSinceClean = 0;
      s_lastCleanMs = millis();
      // r162 HYG3 (P2): discharge the UI grey counter on ANY from-white clean, not
      // only on the one this counter armed. Before this, a cadenceDue clean (24
      // partials / 60 s) or a forced refresh left s_uiGreyPaintsSinceClean where it
      // was: the next grey paint then armed a SECOND from-white clean for residue
      // this one had already cleared. A from-white GC16 drives every pixel, so it is
      // a superset of what either counter measures and both are discharged here.
      const unsigned greyPaintsDischarged = s_uiGreyPaintsSinceClean;
      s_uiGreyPaintsSinceClean = 0;
      char gc[128];
      if (greyCadenceDue) {
        ++s_uiGreyCleans;
        snprintf(gc, sizeof(gc),
                 "[GREY] clean cadence: grey_paints=%u every=%u cleans=%u -> from-white GC16",
                 greyPaintsDischarged + 1, kUiGreyCleanEvery, s_uiGreyCleans);
        cp_panel_note(gc);
        if (g_journal_hook) g_journal_hook(gc);
      } else if (greyPaintsDischarged != 0) {
        snprintf(gc, sizeof(gc), "[GREY] clean (other) discharged grey_paints=%u -> from-white GC16",
                 greyPaintsDischarged);
        cp_panel_note(gc);
        if (g_journal_hook) g_journal_hook(gc);
      }
    }
  } else if (vidDrive) {
    // VID1 bench lane. Whole-panel (MODE_SKIP_UNCHANGED cleared) on purpose: the
    // question is what the 8-phase table costs per frame, and a diff drive of an
    // unchanged frame would return before clocking a single phase -- the LOG1
    // no-op signature. 8 phases x 12 ms = ~96 ms expected; the per-frame line
    // below is the measurement.
    drawErr = epd_hl_update_screen_full(&g_hl, MODE_DU, 25);
  } else if (greyDrive) {
    // ERASE-THEN-GREY, full-panel: the wipe table's erase columns make the drive
    // independent of the old state, and _full clears MODE_SKIP_UNCHANGED so every
    // pixel in the frame is conditioned -- including the paper between glyphs and
    // the ink of a 1 px rule, which is what stops the drift accumulating.
    drawErr = epd_hl_update_screen_full(&g_hl, MODE_GL16, 25);
    if (!drawErr) ++s_partialsSinceClean;
  } else {
    drawErr = epd_hl_update_screen(&g_hl, clean ? MODE_GC16 : MODE_DU, 25);
    if (!clean && !drawErr) ++s_partialsSinceClean;
  }
  const uint32_t tElapsed = millis() - tStart;
  drive_gate(0);  // r129 (H23): the waveform is over -- let the held log lines drain now
  // Restore the default waveform BEFORE anything else touches the state: the
  // 0x400 recovery path below re-attaches &MY_WAVEFORM itself, and every other
  // drive assumes MY_WAVEFORM is resident (it is the only table with DU/GC16).
  if (greyDrive) epd_hl_waveform(&g_hl, &MY_WAVEFORM);
  if (vidDrive) epd_hl_waveform(&g_hl, &MY_WAVEFORM);
  // VID1: one line per armed frame, with the number that decides whether 8 phases
  // is 10 fps or a lie. Decrement only when a frame actually ran.
  if (vidDrive) {
    const int done = s_shortDuPlanned - s_shortDuRemaining + 1;
    char vline[128];
    snprintf(vline, sizeof(vline), "[VID] frame %d/%d lane=shortDU(8) ms=%lu err=0x%x", done,
             s_shortDuPlanned, (unsigned long)tElapsed, (unsigned)drawErr);
    cp_panel_note(vline);
    if (g_journal_hook) g_journal_hook(vline);
    if (s_shortDuRemaining > 0) --s_shortDuRemaining;
    if (s_shortDuRemaining == 0) s_shortDuPlanned = 0;
  }
  mn_note("[rtc] show: update done (ms=%lu) t=%lu ms\n", (unsigned long)tElapsed,
          (unsigned long)(millis()));
  // One line per show(), straight to the journal: waveform, rail verdict, draw result and duration.
  // tps comes from the vendor's own poweron sequence, so a 0 here is the rails failing to come up.
  //
  // LOG1: a duration under kNoDriveMsFloor is the no-op signature (see the constant), so the verb
  // changes to "SKIPPED (no drive)". Field order and names are unchanged, so every existing reader
  // of this line still finds mode=/tps=/err=/ms=; only the word after "show" differs.
  const char* showVerb = (tElapsed < kNoDriveMsFloor) ? "show SKIPPED (no drive)" : "show";
  if (g_journal_hook) {
    char line[192];
    snprintf(line, sizeof(line), "[panel] %s mode=%s tps=%d err=0x%x ms=%lu bin=%d grey=%d levels=%d",
             showVerb, modeName, epd_board_metalio_eink4_plus_tps_ok() ? 1 : 0, (unsigned)drawErr,
             (unsigned long)tElapsed, binaryDuForced ? 1 : 0, frameHasGrey ? 1 : 0, frameLevels);
    g_journal_hook(line);
  }
  // r55 diagnostic: the same line into the app debug log. The journal hook above is only installed
  // during hard power-off, so every other drive - including the wake-time whole-panel refresh that
  // shows striping - was invisible. err=0x400 here means the line queue underran and the drive never
  // covered the panel, which is what banded refresh looks like on glass.
  {
    char note[192];
    snprintf(note, sizeof(note), "[panel] %s mode=%s tps=%d err=0x%x ms=%lu bin=%d grey=%d levels=%d",
             showVerb, modeName, epd_board_metalio_eink4_plus_tps_ok() ? 1 : 0, (unsigned)drawErr,
             (unsigned long)tElapsed, binaryDuForced ? 1 : 0, frameHasGrey ? 1 : 0, frameLevels);
    cp_panel_note(note);
  }

  // r129 (H24): DRIVE instrumentation. LOGGING ONLY -- both lines read counters the drive path
  // already keeps and record what this drive actually ran with: the CPU/mutex state it was clocked
  // under, and the bounce-refill budget against the worst EOF work seen in the last frame. No
  // ordering change, no delay, no register touched. Deliberately NOT routed to the journal: the
  // wake drive capture counts every journal line as a drive, and instrumentation must never look
  // like one.
  {
    int eofGapUs = 0;
    int eofWorkUs = 0;
    epd_lcd_get_stream_stats(&eofGapUs, &eofWorkUs);
    const int pmState = cp_wake_pm_lock_state();
    char dline[192];
    snprintf(dline, sizeof(dline), "[DRIVE] cpu=%d MHz pm=%d apb_lock=%d cpu_lock=%d mode=%s",
             cp_cpu_mhz(), cp_pm_enabled(), pmState & 1, (pmState >> 1) & 1, modeName);
    cp_panel_note(dline);
    snprintf(dline, sizeof(dline),
             "[DRIVE] eof_work_max_us=%d eof_dt_max_us=%d budget_us=%d lines=%d clocked=%d", eofWorkUs,
             eofGapUs, epd_lcd_stream_budget_us(), epd_lcd_stream_lines(), epd_lcd_stream_clocked());
    cp_panel_note(dline);
  }

  // 0x400 = EPD_DRAW_EMPTY_LINE_QUEUE: the line queue never drained, so a frame that "ran" never
  // reached the glass. Recovery is the vendor's own (epd_refresh.cc's EMPTY_LINE_QUEUE branch):
  // re-attach the waveform, run the CHECKED clear (it clocked the clear cycles itself), and only
  // then retry -- and if the clear also fails, a real TPS off/on before clearing once more. The
  // retry must be a from-white drive: after an aborted drive epdiy's back_fb no longer describes
  // the glass, so a diff would target the wrong pixels.
  if (drawErr & EPD_DRAW_EMPTY_LINE_QUEUE) {
    epd_hl_waveform(&g_hl, &MY_WAVEFORM);
    enum EpdDrawError clearErr = epd_clear_checked();
    if (clearErr) {
      epd_poweroff();
      g_powered = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      epd_poweron();
      g_powered = true;
      clearErr = epd_clear_checked();
      if (clearErr) g_powered = false;  // leave the next show() to power the rails itself
    }
    enum EpdDrawMode retryMode = clean ? MODE_GC16 : MODE_DU;
    if (refresh == METALIO_PANEL_REFRESH_GL16) retryMode = MODE_GL16;
    else if (refresh == METALIO_PANEL_REFRESH_FULL) retryMode = MODE_GC16;
    drawErr = epd_hl_update_screen_from_white(&g_hl, retryMode, 25);
    if (g_journal_hook) {
      char rec[144];
      snprintf(rec, sizeof(rec), "[panel] recover 0x400 clear=%s retry=from-white err=0x%x",
               clearErr ? "FAIL" : "ok", (unsigned)drawErr);
      g_journal_hook(rec);
    }
  }
  // Final drive result for this show(), after the 0x400 retry has had its chance.
  // The renderer reads it through metalio_vendor_panel_last_draw_error().
  g_last_draw_err = static_cast<uint32_t>(drawErr);

  // INDEPENDENT RAIL EVIDENCE (PG1). The `tps=` field of the line above is
  // epd_board_metalio_eink4_plus_tps_ok(), a LATCH written by the last power-up: on this build
  // the rails stay up across drives, so that latch can never notice a rail that collapsed DURING
  // a waveform. The TPS PG register is not a substitute — outside the power-up window it reads
  // 0x00 with the rails demonstrably up (live: `panel` reports pg_reg=0x00 rails_up=0x00 while
  // PWR_GOOD=1 and the panel is driving), so a detector keyed to it cannot fire.
  //
  // PWR_GOOD (TCA P07) is the panel's own answer, and it is only meaningful while the rails are
  // SUPPOSED to be up: the TPS drives it open-drain, so a parked or SLEEPing part releases the
  // line and it reads high. That window is exactly here — the waveform has just finished and the
  // rails have not been taken down yet — which is why the sample belongs at the end of show()
  // and not in a probe that runs with the panel parked.
  //
  // Reported as `pgood=1|0|-1` (unknown when the expander read fails) through the same journal
  // hook the drive line uses, so the renderer can use it without a second measurement.
  {
    uint8_t pgEndPort = 0xFF;
    const bool pgEndKnown = epd_board_metalio_eink4_plus_ioexp_read_port(kPgoodPort, &pgEndPort);
    const int pgoodEnd = !pgEndKnown ? -1 : (((pgEndPort & kPgoodBit) != 0) ? 1 : 0);
    char rails[128];
    snprintf(rails, sizeof(rails), "[panel] rails tps=%d pgood=%d err=0x%x (PWR_GOOD at end of waveform)",
             epd_board_metalio_eink4_plus_tps_ok() ? 1 : 0, pgoodEnd, (unsigned)drawErr);
    if (g_journal_hook) g_journal_hook(rails);
    cp_panel_note(rails);
  }

  // Exit edge: a finger that went down during the waveform is still down now, so this poll turns a
  // tap that arrived mid-refresh into a normal press. The duration is stamped so the log records
  // exactly how long input was unobserved.
  if (g_poll_hook) g_poll_hook();


  if (power_off_after) {
    epd_poweroff();
    g_powered = false;
  }
}

extern "C" void metalio_vendor_panel_off(void) {
  if (g_powered) {
    epd_poweroff();
    g_powered = false;
  }
}

extern "C" bool metalio_vendor_panel_repower(void) {
  // The vendor's own reset gap: a real TPS65185 off/on, not just a re-assert of the enable bit.
  // Measured: 48 GC16 phases clocked in 648 ms yet the glass kept the old frame, which only makes
  // sense if the rails were never actually up. Cycling them makes the next drive provable.
  if (g_powered) {
    epd_poweroff();
    g_powered = false;
    delay(20);
  }
  epd_poweron();
  g_powered = true;
  return epd_board_metalio_eink4_plus_tps_ok();
}

extern "C" int metalio_vendor_panel_width(void) { return g_w; }
extern "C" int metalio_vendor_panel_height(void) { return g_h; }

// ---------------------------------------------------------------------------
// READ-ONLY TPS65185 register readout (see the contract in the header).
//
// The row table lives here, not in the app: the shell prints this, but the PMIC
// registers belong to this component. That also fixes the link failure for good —
// the app's probe TU is C++, so a direct tps_read_register() reference comes out
// mangled (_Z17tps_read_register10i2c_port_ti) and no member of the C archive can
// satisfy it. See the header comment for the full rationale.
//
// Every read goes through tps_read_register_ex(), so a bus NAK is reported as a
// failure instead of becoming a plausible-looking 0x00.
// ---------------------------------------------------------------------------
namespace {

struct TpsProbeRow {
  int reg;
  const char* name;
};

const TpsProbeRow kTpsProbeRows[] = {
    {TPS_REG_TMST_VALUE, "TMST_VALUE"}, {TPS_REG_ENABLE, "ENABLE"},   {TPS_REG_VADJ, "VADJ"},
    {TPS_REG_VCOM1, "VCOM1"},           {TPS_REG_VCOM2, "VCOM2"},     {TPS_REG_INT_EN1, "INT_EN1"},
    {TPS_REG_INT_EN2, "INT_EN2"},       {TPS_REG_INT1, "INT1"},       {TPS_REG_INT2, "INT2"},
    {TPS_REG_UPSEQ0, "UPSEQ0"},         {TPS_REG_UPSEQ1, "UPSEQ1"},   {TPS_REG_DWNSEQ0, "DWNSEQ0"},
    {TPS_REG_DWNSEQ1, "DWNSEQ1"},       {TPS_REG_TMST1, "TMST1"},     {TPS_REG_TMST2, "TMST2"},
    {TPS_REG_PG, "PG"},                 {TPS_REG_REVID, "REVID"},
};

constexpr int kTpsProbeRowCount = (int)(sizeof(kTpsProbeRows) / sizeof(kTpsProbeRows[0]));

// r91: the bus-lock timeout for the debug-shell read paths. Same 100 ms the bus
// shims use (pca9555.c / tps65185.c define I2C1_LOCK_MS locally, so it is not
// visible in this TU). A lock we cannot take in 100 ms means the panel task is
// mid-transfer, which is exactly when we must NOT read the PMIC unlocked.
constexpr int kI2c1LockMs = 100;

// How long a read-only probe waits for the TPS to answer after WAKEUP. Manual 8.4
// gives t_WAKE ~10 ms from SLEEP; the margin is for a part whose rails were just
// cycled (CP_EPD_POWERDOWN_AFTER_DRIVE) and for a bus that is being handed over.
constexpr int kTpsWakeMs = 120;


void tpsProbeAppend(char* out, size_t cap, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

void tpsProbeAppend(char* out, size_t cap, const char* fmt, ...) {
  if (out == nullptr || cap == 0) return;
  const size_t len = strlen(out);
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out + len, cap - len, fmt, ap);
  va_end(ap);
}

}  // namespace

extern "C" int metalio_vendor_panel_tps_read_pg(unsigned char* pg) {
  if (pg == nullptr) return (int)ESP_ERR_INVALID_ARG;
  // The bus lock is MANDATORY here: tps_read_register_ex() locks per transfer, so
  // this task must own the bus for the WAKE and READ pair -- a panel-task transfer
  // landing between the WAKEUP write and the read would return a NAK that has
  // nothing to do with the PMIC. The mutex is recursive, so the nested takes inside
  // tps_probe()/tps_read_register_ex() are free.
  if (!metalio_i2c1_lock(kI2c1LockMs)) return (int)ESP_ERR_TIMEOUT;
  // After a drive the TPS is in SLEEP and does not ACK (manual 8.4): wake it to
  // STANDBY, read, then put the power state back exactly as it was.
  epd_board_metalio_eink4_plus_tps_wake_for_read(kTpsWakeMs, nullptr, nullptr);
  uint8_t v = 0;
  const esp_err_t err = tps_read_register_ex(I2C_NUM_1, TPS_REG_PG, &v);
  epd_board_metalio_eink4_plus_tps_read_restore();
  metalio_i2c1_unlock();
  if (err == ESP_OK) *pg = v;
  return (int)err;
}

extern "C" int metalio_vendor_panel_tps_dump(char* out, size_t cap, MetalioTpsDump* summary) {
  if (out != nullptr && cap > 0) out[0] = '\0';

  int failed = 0;
  int pgOk = 0;
  uint8_t pg = 0xFF;
  bool wasAsleep = false;
  bool woken = false;
  unsigned waitedMs = 0;

  // One lock for the whole sweep: the WAKEUP write, the 17 reads and the restore
  // must be one uninterrupted sequence, and this runs on the debug-shell task,
  // concurrent with the panel task.
  if (!metalio_i2c1_lock(kI2c1LockMs)) {
    tpsProbeAppend(out, cap, "  (I2C bus busy: lock timeout)\n");
    if (summary) {
      summary->total = kTpsProbeRowCount;
      summary->failed = kTpsProbeRowCount;
      summary->pg_ok = 0;
      summary->pg = 0xFF;
      summary->was_asleep = 0;
      summary->woken = 0;
      summary->wake_ms = 0;
    }
    return (int)ESP_ERR_TIMEOUT;
  }

  // THE TRAP THIS FIXES: after every drive (CP_EPD_POWERDOWN_AFTER_DRIVE) and on
  // standby the board drives PWRUP/VCOM/WAKEUP low, so the TPS sits in SLEEP and
  // does not ACK I2C at all. Every register then reports ESP_ERR_INVALID_RESPONSE,
  // which reads like a dead bus but is a powered-down PMIC. Wake to STANDBY first
  // and restore afterwards -- the guard never raises PWRUP on its own.
  woken = epd_board_metalio_eink4_plus_tps_wake_for_read(kTpsWakeMs, &wasAsleep, &waitedMs);

  for (int i = 0; i < kTpsProbeRowCount; ++i) {
    const TpsProbeRow& row = kTpsProbeRows[i];
    uint8_t v = 0;
    const esp_err_t err = tps_read_register_ex(I2C_NUM_1, row.reg, &v);
    if (err == ESP_OK) {
      tpsProbeAppend(out, cap, "  0x%02X %-11s = 0x%02X  %3u\n", row.reg, row.name, v,
                     (unsigned)v);
      if (row.reg == TPS_REG_PG) {
        pgOk = 1;
        pg = v;
      }
    } else {
      ++failed;
      tpsProbeAppend(out, cap, "  0x%02X %-11s = ----  %s\n", row.reg, row.name,
                     esp_err_to_name(err));
    }
  }

  epd_board_metalio_eink4_plus_tps_read_restore();
  metalio_i2c1_unlock();

  if (summary != nullptr) {
    summary->total = kTpsProbeRowCount;
    summary->failed = failed;
    summary->pg_ok = pgOk;
    summary->pg = pg;
    summary->was_asleep = wasAsleep ? 1 : 0;
    summary->woken = woken ? 1 : 0;
    summary->wake_ms = (int)waitedMs;
  }
  return kTpsProbeRowCount;
}

// ---------------------------------------------------------------------------
// Fault telemetry (3.9). The contract is in the header: read-only, and the read
// IS the acknowledge on this part, so one sample is the record.
// ---------------------------------------------------------------------------
extern "C" const char* metalio_vendor_panel_tps_int1_short(int bit) {
  switch (bit) {
    case 7:
      return "VCOM";
    case 6:
      return "VNEG";
    case 5:
      return "VPOS";
    case 4:
      return "VGL";
    case 3:
      return "VGH";
    case 1:
      return "ACQC";
    default:
      return nullptr;
  }
}

extern "C" int metalio_vendor_panel_tps_faults(MetalioTpsFaults* out, char* text, size_t cap) {
  if (text != nullptr && cap > 0) text[0] = '\0';

  MetalioTpsFaults f = {};
  int rc = 0;

  if (!metalio_i2c1_lock(kI2c1LockMs)) {
    tpsProbeAppend(text, cap, "  (I2C bus busy: lock timeout -- no sample taken)\n");
    if (out != nullptr) *out = f;
    return (int)ESP_ERR_TIMEOUT;
  }

  // Same wake-then-restore guard as the register dump: the part is parked in SLEEP
  // after every drive and does not ACK there, and the restore never raises PWRUP.
  bool wasAsleep = false;
  unsigned waitedMs = 0;
  f.woken = epd_board_metalio_eink4_plus_tps_wake_for_read(kTpsWakeMs, &wasAsleep, &waitedMs) ? 1 : 0;
  f.was_asleep = wasAsleep ? 1 : 0;
  f.wake_ms = (int)waitedMs;

  uint8_t v = 0;
  const esp_err_t e1 = tps_read_register_ex(I2C_NUM_1, TPS_REG_INT1, &v);
  if (e1 == ESP_OK) {
    f.int1_ok = 1;
    f.int1 = v;
  } else {
    rc = (int)e1;
  }
  const esp_err_t e2 = tps_read_register_ex(I2C_NUM_1, TPS_REG_INT2, &v);
  if (e2 == ESP_OK) {
    f.int2_ok = 1;
    f.int2 = v;
  }
  const esp_err_t epg = tps_read_register_ex(I2C_NUM_1, TPS_REG_PG, &v);
  if (epg == ESP_OK) {
    f.pg_ok = 1;
    f.pg = v;
  }

  epd_board_metalio_eink4_plus_tps_read_restore();
  metalio_i2c1_unlock();

  int named = 0;
  for (int b = 0; b < 8; ++b) {
    if (metalio_vendor_panel_tps_int1_short(b) != nullptr && (f.int1 & (1u << b)) != 0) ++named;
  }
  f.named_bits = named;

  tpsProbeAppend(text, cap, "  faults: one read-only sample (no register written)\n");
  if (f.int1_ok) {
    tpsProbeAppend(text, cap, "  INT1 = 0x%02X", f.int1);
    if (named == 0) {
      tpsProbeAppend(text, cap, "  (no named bit set)");
    } else {
      tpsProbeAppend(text, cap, "  ");
      bool first = true;
      for (int b = 7; b >= 0; --b) {
        const char* n = metalio_vendor_panel_tps_int1_short(b);
        if (n == nullptr || (f.int1 & (1u << b)) == 0) continue;
        tpsProbeAppend(text, cap, "%s%s (bit%d)", first ? "" : ", ", n, b);
        first = false;
      }
    }
    tpsProbeAppend(text, cap, "\n");
  } else {
    tpsProbeAppend(text, cap, "  INT1 = ----  %s\n", esp_err_to_name((esp_err_t)rc));
  }
  if (f.int2_ok) {
    tpsProbeAppend(text, cap, "  INT2 = 0x%02X  (no bit map in this tree: raw only)\n", f.int2);
  } else {
    tpsProbeAppend(text, cap, "  INT2 = ----  %s\n", esp_err_to_name(e2));
  }
  if (f.pg_ok) {
    tpsProbeAppend(text, cap, "  PG   = 0x%02X  rails_up=0x%02X (vendor wants 0xFA)\n", f.pg,
                   static_cast<unsigned>(f.pg & 0xFA));
  } else {
    tpsProbeAppend(text, cap, "  PG   = ----  %s\n", esp_err_to_name(epg));
  }
  if (f.was_asleep) {
    tpsProbeAppend(text, cap,
                   "  power: PMIC was ASLEEP; %s, original power state restored (rails unchanged)\n",
                   f.woken ? "it answered after WAKEUP" : "it did NOT answer after WAKEUP");
  }
  tpsProbeAppend(text, cap,
                 "  latch: reading INT1/INT2 IS the acknowledge on this part (bus/tps65185.c\n"
                 "         tps_vcom_kickback_start() reads INT1 to clear a stale ACQC), so these\n"
                 "         flags were pending until this sample and are consumed by it. Nothing is\n"
                 "         written and nothing is left latched by us; the sample above is the record.\n");
  if (out != nullptr) *out = f;
  return rc;
}

// --- VCOM / black depth (D9) -------------------------------------------------
// The driver's own copy of the value the power-on sequence writes. A plain store on
// purpose: nothing here touches the bus, so it cannot collide with a scan. The one
// write is epd_board_poweron()'s tps_set_vcom(vcom) -- rails ramping, output enable
// still LOW -- which is the between-scans window the setting is allowed to use.
namespace {
int g_vcom_mv = EPD_VCOM_MV;
}  // namespace

extern "C" int metalio_vendor_panel_set_vcom_mv(int mv) {
  // The vendor's three black-depth levels, and only those: an inferred or widened
  // VCOM is a way to damage the panel, not a feature.
  if (mv != 1200 && mv != 1600 && mv != 2000) return -EINVAL;
  g_vcom_mv = mv;
  epd_set_vcom(static_cast<uint16_t>(mv));  // board-side store; the bus write is at power-on
  return 0;
}

extern "C" int metalio_vendor_panel_vcom_mv(void) { return g_vcom_mv; }

// ---------------------------------------------------------------------------
// Arduino-Wire-backed I2C for the vendor shims.
//
// The direct i2c-ng path fails on this board while Wire succeeds (a Wire scan
// ACKs 0x19, 0x20, 0x38, 0x51, 0x55 and 0x6B), so pca9555.c / tps65185.c
// transfer through these. metalio_i2c1_lock() serialises them.
// ---------------------------------------------------------------------------
extern "C" esp_err_t mn_wire_write(uint8_t addr7, const uint8_t* data, size_t len) {
  Wire.beginTransmission(addr7);
  if (len) Wire.write(data, len);
  const uint8_t rc = Wire.endTransmission();  // true STOP
  if (rc == 0) return ESP_OK;
  if (rc == 2) return ESP_ERR_NOT_FOUND;      // address NACK
  if (rc == 3) return ESP_ERR_INVALID_RESPONSE;
  if (rc == 5) return ESP_ERR_TIMEOUT;
  return ESP_FAIL;
}

extern "C" esp_err_t mn_wire_write_read(uint8_t addr7, const uint8_t* w, size_t wlen,
                                        uint8_t* r, size_t rlen) {
  Wire.beginTransmission(addr7);
  if (wlen) Wire.write(w, wlen);
  const uint8_t rc = Wire.endTransmission(false);  // repeated START, no STOP
  if (rc != 0) {
    if (rc == 2) return ESP_ERR_NOT_FOUND;
    if (rc == 3) return ESP_ERR_INVALID_RESPONSE;
    if (rc == 5) return ESP_ERR_TIMEOUT;
    return ESP_FAIL;
  }
  const size_t got = Wire.requestFrom((int)addr7, (int)rlen);
  if (got != rlen) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i < rlen; ++i) r[i] = (uint8_t)Wire.read();
  return ESP_OK;
}

extern "C" esp_err_t mn_wire_probe(uint8_t addr7) {
  Wire.beginTransmission(addr7);
  const uint8_t rc = Wire.endTransmission();
  return rc == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// ONE BUS, ONE LOCK. The vendor shims serialise via metalio_i2c1_lock(); the
// app-side Wire users (touch, frontlight, gauge) take no lock, so a short app
// transaction can land inside a vendor multi-step sequence. Exposing the same
// recursive mutex lets board code bracket its sequences too.
// ---------------------------------------------------------------------------
extern "C" bool mn_bus_lock(uint32_t timeout_ms) {
  return metalio_i2c1_lock((int)timeout_ms);
}

extern "C" void mn_bus_unlock(void) {
  metalio_i2c1_unlock();
}

// 9-clock bus recovery, for use BEFORE Wire.begin(). A slave interrupted
// mid-byte can hold SDA low (GPIO0 is also the ROM strap). Doing this AFTER a
// controller owns the pins is fatal: gpio_config() detaches them and every
// transfer fails -- that was the "TCA9555 not found" fault.
extern "C" void mn_bus_recover(void) {
  const gpio_config_t conf = {
      .pin_bit_mask = (1ULL << 0) | (1ULL << 1),
      .mode = GPIO_MODE_INPUT_OUTPUT_OD,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&conf);
  gpio_set_level((gpio_num_t)0, 1);
  gpio_set_level((gpio_num_t)1, 1);
  esp_rom_delay_us(20);
  for (int i = 0; i < 9 && gpio_get_level((gpio_num_t)0) == 0; ++i) {
    gpio_set_level((gpio_num_t)1, 0); esp_rom_delay_us(5);
    gpio_set_level((gpio_num_t)1, 1); esp_rom_delay_us(5);
  }
  gpio_set_level((gpio_num_t)0, 0); esp_rom_delay_us(5);
  gpio_set_level((gpio_num_t)1, 1); esp_rom_delay_us(5);
  gpio_set_level((gpio_num_t)0, 1); esp_rom_delay_us(5);
}

// ---------------------------------------------------------------------------
// RTC-backed boot log. Everything after GPIO19 is claimed is invisible over
// USB, and this buffer has been measured to survive a USB/software reset.
// Dumped at the next setup(), before GPIO19 is touched.
// ---------------------------------------------------------------------------
#define MN_RTC_LOG_CAP 3072
RTC_NOINIT_ATTR static char s_rtc_log[MN_RTC_LOG_CAP];
RTC_NOINIT_ATTR static uint16_t s_rtc_len;
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
static const uint32_t MN_RTC_MAGIC = 0x434E5031u;  // "CNP1"

extern "C" void mn_note(const char* fmt, ...) {
  if (s_rtc_magic != MN_RTC_MAGIC) {
    s_rtc_magic = MN_RTC_MAGIC;
    s_rtc_len = 0;
  }
  if (s_rtc_len + 96 >= MN_RTC_LOG_CAP) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(&s_rtc_log[s_rtc_len], MN_RTC_LOG_CAP - s_rtc_len, fmt, ap);
  va_end(ap);
  if (n > 0) s_rtc_len += (uint16_t)n;
  if (s_rtc_len >= MN_RTC_LOG_CAP) s_rtc_len = MN_RTC_LOG_CAP - 1;
}

extern "C" void mn_note_dump(void) {
  if (s_rtc_magic != MN_RTC_MAGIC || s_rtc_len == 0) {
    printf("[rtc] no previous log (magic=0x%08X want=0x%08X len=%u) -- RTC NOINIT "
           "not retained across this reset\n", s_rtc_magic, MN_RTC_MAGIC, (unsigned)s_rtc_len);
    fflush(stdout);
    return;
  }
  printf("[rtc] === previous boot's post-GPIO19 log (%u bytes) ===\n", (unsigned)s_rtc_len);
  fwrite(s_rtc_log, 1, s_rtc_len, stdout);
  printf("\n[rtc] === end ===\n");
  fflush(stdout);
  s_rtc_len = 0;  // consumed
}

// In-band stage signal: the only channel that survives GPIO19 being claimed.
//   1 pulse  = expander probe OK
//   2 pulses = epd_poweron() returned
//   3 SLOW pulses = probe failed (distinct duration, so it cannot be miscounted)
extern "C" void mn_stage_signal(int n) {
#ifdef CP_STAGE_SIGNAL
  pinMode(6, OUTPUT);  // COOL
  pinMode(7, OUTPUT);  // WARM
  digitalWrite(6, LOW);
  for (int i = 0; i < n; ++i) {
    digitalWrite(7, HIGH); delay(120);
    digitalWrite(7, LOW);  delay(120);
  }
  digitalWrite(6, HIGH);
#else
  (void)n;
#endif
}

extern "C" void mn_stage_signal_slow(int n) {
#ifdef CP_STAGE_SIGNAL
  pinMode(6, OUTPUT);
  pinMode(7, OUTPUT);
  digitalWrite(6, LOW);
  for (int i = 0; i < n; ++i) {
    digitalWrite(7, HIGH); delay(500);
    digitalWrite(7, LOW);  delay(300);
  }
  digitalWrite(6, HIGH);
#else
  (void)n;
#endif
}

// ---------------------------------------------------------------------------
// CALIBRATION CROSSHAIR. Each tap draws a cross at the coordinate the firmware
// computed, then pushes the frame. Comparing that cross with where the finger
// actually landed turns an invisible mapping bug into a photograph.
// ---------------------------------------------------------------------------
static int s_markX = -1;
static int s_markY = -1;

static void draw_cross(uint8_t* fb, int W, int H, int S, int cx, int cy) {
  auto px = [&](int x, int y, uint8_t g) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    put4(fb, S, x, y, g);   // raw landscape coordinates (EPD_ROT_LANDSCAPE is identity)
  };
  const int arm = 40;
  for (int d = -arm; d <= arm; ++d) {
    for (int w = -1; w <= 1; ++w) {
      px(cx + d, cy + w, 0x00);
      px(cx + w, cy + d, 0x00);
    }
  }
  // A ring so the centre is unmistakable even on a busy UI.
  for (int a = 0; a < 360; a += 6) {
    const float r = 60.0f;
    px(cx + (int)(r * cosf(a * 3.14159265f / 180.0f)),
       cy + (int)(r * sinf(a * 3.14159265f / 180.0f)), 0x00);
  }
}

extern "C" void metalio_vendor_panel_mark_flush(int x, int y) {
  s_markX = x;
  s_markY = y;
  if (!g_begun || !g_hl.front_fb) return;
  draw_cross(g_hl.front_fb, g_w, g_h, g_stride4, x, y);
  if (!g_powered) {
    epd_poweron();
    g_powered = true;
  }
  epd_hl_update_screen(&g_hl, MODE_GC16, 25);
  mn_note("[mark] flushed cross at (%d,%d)\n", x, y);
}

// ---------------------------------------------------------------------------
// VENDOR I2C API, backed by Arduino Wire.
//
// The transplanted vendor files (pca9555.c, tps65185.c, epd_board_*) call the
// metalio_i2c1_* API. Upstream implements it with i2c_master on I2C_NUM_1; here it
// is implemented on the ONE bus Arduino Wire owns, so vendor sources can be copied
// verbatim while the board keeps a single I2C master. metalio_i2c1_lock() (the
// vendor mutex) still serialises every sequence.
// ---------------------------------------------------------------------------
extern "C" esp_err_t metalio_i2c1_init(gpio_num_t sda, gpio_num_t scl) {
  static bool begun = false;
  if (!begun) {
    if (!Wire.begin((int)sda, (int)scl, 100000)) {
      // Already-created bus: keep using it rather than failing the boot.
      printf("[i2c] Wire.begin(%d,%d) returned false; assuming an existing bus\n", (int)sda, (int)scl);
      fflush(stdout);
    }
    // BQ27220 stretches SCL; upstream sets scl_wait_us = 10000 for exactly this.
    Wire.setTimeOut(50);
    begun = true;
  }
  return ESP_OK;
}

extern "C" esp_err_t metalio_i2c1_deinit(void) { return ESP_OK; }  // never drop Wire

extern "C" bool metalio_i2c1_lock_for_flash(int timeout_ms) { return metalio_i2c1_lock(timeout_ms); }

extern "C" esp_err_t metalio_i2c1_device_locked(i2c_port_t port, uint8_t addr,
                                                i2c_master_dev_handle_t* device) {
  (void)port; (void)addr;
  if (device) *device = NULL;
  return ESP_ERR_NOT_SUPPORTED;  // address-based calls below are used instead
}

extern "C" esp_err_t metalio_i2c1_probe_locked(i2c_port_t port, uint8_t addr, int timeout_ms) {
  (void)port; (void)timeout_ms;
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

extern "C" esp_err_t metalio_i2c1_transmit_locked(i2c_port_t port, uint8_t addr,
                                                  const uint8_t* data, size_t len, int timeout_ms) {
  (void)port; (void)timeout_ms;
  Wire.beginTransmission(addr);
  if (len) Wire.write(data, len);
  switch (Wire.endTransmission()) {
    case 0: return ESP_OK;
    case 2: return ESP_ERR_NOT_FOUND;
    case 3: return ESP_ERR_INVALID_RESPONSE;
    case 5: return ESP_ERR_TIMEOUT;
    default: return ESP_FAIL;
  }
}

extern "C" esp_err_t metalio_i2c1_receive_locked(i2c_port_t port, uint8_t addr,
                                                 uint8_t* data, size_t len, int timeout_ms) {
  (void)port; (void)timeout_ms;
  const size_t got = Wire.requestFrom((int)addr, (int)len);
  if (got != len) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i < len; ++i) data[i] = (uint8_t)Wire.read();
  return ESP_OK;
}

extern "C" esp_err_t metalio_i2c1_transmit_receive_locked(i2c_port_t port, uint8_t addr,
    const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_len, int timeout_ms) {
  (void)port; (void)timeout_ms;
  Wire.beginTransmission(addr);
  if (tx_len) Wire.write(tx, tx_len);
  const uint8_t rc = Wire.endTransmission(false);  // repeated START
  if (rc != 0) return rc == 2 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
  const size_t got = Wire.requestFrom((int)addr, (int)rx_len);
  if (got != rx_len) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i < rx_len; ++i) rx[i] = (uint8_t)Wire.read();
  return ESP_OK;
}

extern "C" esp_err_t metalio_i2c1_write_reg_locked(i2c_port_t port, uint8_t addr, uint8_t reg,
    const uint8_t* data, size_t len, int timeout_ms) {
  (void)port; (void)timeout_ms;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (len) Wire.write(data, len);
  return Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t metalio_i2c1_read_reg_stop_locked(i2c_port_t port, uint8_t addr, uint8_t reg,
    uint8_t* data, size_t len, int timeout_ms) {
  (void)port; (void)timeout_ms;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return ESP_FAIL;  // full STOP, as vendor expects
  const size_t got = Wire.requestFrom((int)addr, (int)len);
  if (got != len) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i < len; ++i) data[i] = (uint8_t)Wire.read();
  return ESP_OK;
}

// VID1 — arm the 8-phase short-DU table for the next `frames` FAST paints.
//
// THE ONLY CALLER IS `exp vid N` (src/debug/UsbShell.cpp -> shellExp in src/main.cpp),
// which is itself behind debugflag::enabled(). With frames == 0 nothing changes: the
// countdown is a static int initialised to 0 at every boot, so a device that never
// types the command executes exactly the pre-batch drive path. Selecting the table
// is one pointer swap per selected frame, restored immediately after the drive.
extern "C" void metalio_vendor_panel_short_du_frames(int frames) {
  if (frames <= 0) {
    s_shortDuRemaining = 0;
    s_shortDuPlanned = 0;
    return;
  }
  s_shortDuRemaining = frames;
  s_shortDuPlanned = frames;
}

extern "C" int metalio_vendor_panel_short_du_remaining(void) { return s_shortDuRemaining; }

// FLP2 — "the next `frames` show() calls carry binary content".
//
// CALLER: GfxRenderer::displayBuffer()/displayBufferAsync(), through a WEAK
// reference (lib/GfxRenderer/GfxRenderer.cpp), for exactly the paint armed by
// FrontlightPanelActivity::requestAdjustRepaint() (`plan(Adjust).binaryFast`).
// That is the same one-shot discipline as the FLP1 completion suppression and
// it exists for one reason: the panel's frame is the screen underneath plus a
// 1-bit card, so the shim's content-based lanes (grey wipe, >=5-level
// from-white, 24-partial cadence clean) were choosing whole-panel waveforms for
// a paint that only changed a slider and a percentage label.
//
// frames <= 0 disarms. The countdown is a static int initialised to 0, so a
// device that never opens the frontlight panel executes the pre-FLP2 decision
// path byte for byte.
extern "C" void metalio_vendor_panel_binary_du_frames(int frames) {
  if (frames <= 0) {
    s_binaryDuRemaining = 0;
    return;
  }
  s_binaryDuRemaining = frames;
}
