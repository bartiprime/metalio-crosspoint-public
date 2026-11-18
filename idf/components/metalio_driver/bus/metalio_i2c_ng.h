#pragma once

// i2c-ng bridge for the vendor EPDiy bus code.
//
// WHY THIS EXISTS
//   The vendor display stack (bus/tps65185.c, bus/pca9555.c,
//   display/epd_board_metalio_eink4_plus.c) was written against the LEGACY
//   driver/i2c.h API on I2C_NUM_0. Arduino's Wire (esp32 core 3.x) links the NEW
//   i2c-ng driver. IDF aborts at startup as soon as both are merely linked:
//       E i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
//       abort() was called at PC 0x4020d515 on core 0
//   Setting CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK only silenced the abort: both
//   drivers still owned port 0, so every Wire transfer failed with
//   ESP_ERR_INVALID_RESPONSE (264) -- 632 failures in 15 s of idle.
//
//   These helpers let the vendor code use i2c-ng too, so ONE driver owns the
//   bus and Wire works normally. The vendor protocol code above the helpers is
//   not modified.

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create (or adopt) the i2c-ng master bus on the console's port 0 with the
// board's SDA=0 / SCL=1 pins. Idempotent.
esp_err_t mn_i2c_init(void);

esp_err_t mn_i2c_write(uint8_t addr7, const uint8_t* data, size_t len);
esp_err_t mn_i2c_read(uint8_t addr7, uint8_t* data, size_t len);
esp_err_t mn_i2c_write_read(uint8_t addr7, const uint8_t* w, size_t wlen, uint8_t* r, size_t rlen);
esp_err_t mn_i2c_probe(uint8_t addr7);

// Arduino-Wire-backed transfers. The plain i2c-ng path above fails on this board
// while Wire.succeeds (Wire scan ACKs 0x19/0x20/0x38/0x51/0x55/0x6B), so the vendor
// shims talk through Wire instead. One master owns the bus; the shims serialise with
// metalio_i2c1_lock(). Implemented in metalio_vendor_panel.cpp.
esp_err_t mn_wire_write(uint8_t addr7, const uint8_t* data, size_t len);
esp_err_t mn_wire_write_read(uint8_t addr7, const uint8_t* w, size_t wlen, uint8_t* r, size_t rlen);
esp_err_t mn_wire_probe(uint8_t addr7);

#ifdef __cplusplus
}
#endif
