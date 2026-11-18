// DISABLED BY DEFAULT. This bridge creates an i2c-ng master, and a SECOND master on
// SDA=0/SCL=1 is exactly the fault that produced `TCA9555 not found`, a blank panel
// and a 48 s boot. It is kept only as a record of the wrong approach; build with
// -DCP_I2C_NG_EXPERIMENT to compile it, and only if you have removed Wire from the bus.
#if defined(CP_I2C_NG_EXPERIMENT)
// i2c-ng bridge for the vendor EPDiy bus code -- see metalio_i2c_ng.h for why.
//
// Port 0, SDA=GPIO0, SCL=GPIO1, 100 kHz: 100 kHz rather than 400 kHz because the
// vendor's own comment says the bus is more reliable at 100 kHz and the BQ27220
// gauge can stretch the clock to ~4 ms.

#include "metalio_i2c_ng.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "driver/i2c_master.h"
#include "esp_log.h"

#define MN_I2C_PORT   0
#define MN_I2C_SDA    0
#define MN_I2C_SCL    1
#define MN_I2C_HZ     100000
#define MN_I2C_TIMEOUT_MS 2000

static const char* TAG = "mn_i2c";
static i2c_master_bus_handle_t s_bus = NULL;

// i2c-ng requires a device handle per address. Cache a handful; the board talks
// to the TPS65185 (0x68), TCA9555 (0x20), BQ27220 (0x55), PCF8563 (0x51) and a
// couple of codec/IMU addresses.
typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t h;
} dev_slot_t;

static dev_slot_t s_devs[8];
static size_t s_ndev = 0;

esp_err_t mn_i2c_init(void) {
    if (s_bus != NULL) return ESP_OK;

    i2c_master_bus_config_t cfg = {
        .i2c_port = MN_I2C_PORT,
        .sda_io_num = MN_I2C_SDA,
        .scl_io_num = MN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        // Arduino's Wire already created this bus; adopt it instead of creating a
        // second one (two masters on one bus is exactly the bug being fixed).
        err = i2c_master_get_bus_handle(MN_I2C_PORT, &s_bus);
        if (err == ESP_OK) ESP_LOGI(TAG, "adopted existing i2c-ng bus on port %d", MN_I2C_PORT);
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "created i2c-ng bus port=%d sda=%d scl=%d %dHz",
                 MN_I2C_PORT, MN_I2C_SDA, MN_I2C_SCL, MN_I2C_HZ);
    } else {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        s_bus = NULL;
    }
    return err == ESP_OK ? ESP_OK : err;
}

static i2c_master_dev_handle_t dev_for(uint8_t addr7) {
    for (size_t i = 0; i < s_ndev; ++i) {
        if (s_devs[i].addr == addr7) return s_devs[i].h;
    }
    if (s_ndev >= sizeof(s_devs) / sizeof(s_devs[0])) return NULL;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = MN_I2C_HZ,
    };
    i2c_master_dev_handle_t h = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dcfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x%02X: %s", addr7, esp_err_to_name(err));
        return NULL;
    }
    s_devs[s_ndev].addr = addr7;
    s_devs[s_ndev].h = h;
    s_ndev++;
    return h;
}

esp_err_t mn_i2c_write(uint8_t addr7, const uint8_t* data, size_t len) {
    if (mn_i2c_init() != ESP_OK) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t h = dev_for(addr7);
    if (!h) return ESP_ERR_NOT_FOUND;
    return i2c_master_transmit(h, data, len, MN_I2C_TIMEOUT_MS);
}

esp_err_t mn_i2c_read(uint8_t addr7, uint8_t* data, size_t len) {
    if (mn_i2c_init() != ESP_OK) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t h = dev_for(addr7);
    if (!h) return ESP_ERR_NOT_FOUND;
    return i2c_master_receive(h, data, len, MN_I2C_TIMEOUT_MS);
}

esp_err_t mn_i2c_write_read(uint8_t addr7, const uint8_t* w, size_t wlen, uint8_t* r, size_t rlen) {
    if (mn_i2c_init() != ESP_OK) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t h = dev_for(addr7);
    if (!h) return ESP_ERR_NOT_FOUND;
    return i2c_master_transmit_receive(h, w, wlen, r, rlen, MN_I2C_TIMEOUT_MS);
}

esp_err_t mn_i2c_probe(uint8_t addr7) {
    if (mn_i2c_init() != ESP_OK) return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(s_bus, addr7, 100);
}

#endif  // CP_I2C_NG_EXPERIMENT
