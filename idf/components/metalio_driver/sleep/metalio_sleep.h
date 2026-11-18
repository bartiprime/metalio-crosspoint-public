#pragma once

#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Port of the vendor's power_policy/s31_sleep shim (ESP32-S31 only).
 *
 * Call these around every manual esp_light_sleep_start() on this board:
 *   metalio_sleep_begin_cycle();
 *   metalio_sleep_checkpoint(METALIO_SLP_PHASE_BEFORE_SLEEP, applied);
 *   metalio_sleep_light_sleep(applied);      // wraps esp_light_sleep_start()
 *
 * The shim keeps the MPLL/analog-bias restore and the 200 MHz octal PSRAM clock
 * handoff working across a sleep slice, which the stock SDK does not do for this
 * SoC revision. See metalio_sleep.c. */

typedef enum {
  METALIO_SLP_PHASE_ADMISSION = 1,
  METALIO_SLP_PHASE_HARDWARE_PARKED = 2,
  METALIO_SLP_PHASE_WAKE_ARMED = 3,
  METALIO_SLP_PHASE_BEFORE_SLEEP = 4,
  METALIO_SLP_PHASE_AFTER_SLEEP = 5,
  METALIO_SLP_PHASE_KEYS_RESTORED = 6,
  METALIO_SLP_PHASE_BEFORE_SD_RESTORE = 7,
  METALIO_SLP_PHASE_BEFORE_BUS_RESTORE = 8,
  METALIO_SLP_PHASE_RESTORED = 9,
} metalio_slp_phase_t;

esp_err_t metalio_sleep_init(void);
/* Boot: snapshot the previous cycle into the replay slot, clear the trace. */
void metalio_sleep_capture_reset(void);
/* Boot: replay the previous-boot snapshot at 10/30/60 s (USB-JTAG enumeration). */
void metalio_sleep_report_reset(void);
void metalio_sleep_begin_cycle(void);
void metalio_sleep_checkpoint(int phase, unsigned applied);
esp_err_t metalio_sleep_light_sleep(unsigned applied);
uint32_t metalio_sleep_cycle(void);
uint32_t metalio_sleep_last_phase(void);

#ifdef __cplusplus
}
#endif
