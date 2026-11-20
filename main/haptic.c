// DRV2605L on the shared I2C bus (touch is on the same bus).
#include "haptic.h"

#include "board.h"
#include "esp_log.h"

static const char *TAG = "haptic";
static i2c_master_dev_handle_t s_dev;

#define DRV_ADDR 0x5A

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static int rd(uint8_t reg)
{
    uint8_t v;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, 50) != ESP_OK) return -1;
    return v;
}

bool haptic_init(void)
{
    if (i2c_master_probe(board_i2c(), DRV_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no DRV2605L at 0x%02x", DRV_ADDR);
        return false;
    }
    i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = DRV_ADDR, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(board_i2c(), &cfg, &s_dev) != ESP_OK) return false;
    int status = rd(0x00);
    ESP_LOGI(TAG, "DRV2605 status 0x%02x (device id %d)", status, status >> 5);
    wr(0x01, 0x00);                       // out of standby, internal trigger
    wr(0x1A, (rd(0x1A) & 0x7F) | 0x80);   // N_ERM_LRA = LRA
    wr(0x1D, rd(0x1D) | 0x01);            // LRA open loop: no auto-calibration needed
    wr(0x03, 6);                          // ROM library 6 = LRA
    return true;
}

void haptic_play(uint8_t effect)
{
    if (!s_dev) return;
    wr(0x04, effect);
    wr(0x05, 0);
    wr(0x0C, 1);
}
