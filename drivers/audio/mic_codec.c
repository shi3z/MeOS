/* ES7210 register sequence adapted from Espressif esp_codec_dev/device/es7210
 * (Copyright 2023 Espressif Systems (Shanghai) CO LTD, Apache-2.0).
 * See tools/microphone/README.md and LICENSE.es7210 for provenance.
 * Fixed slave, four-slot 16-bit Philips TDM, 12dB gain; physical mapping unqualified. */
#include "mic_internal.h"
#include <m5os/hal_i2c.h>
#define ADDR 0x40
static bool reg(uint8_t r, uint8_t v) { return hal_i2c_sys_write_reg(ADDR, r, v); }
bool mic_codec_start(void)
{
    static const uint8_t setup[][2] = {
        {0x00,0xff}, {0x00,0x41}, {0x01,0x3f}, {0x09,0x30}, {0x0a,0x30},
        {0x23,0x2a}, {0x22,0x0a}, {0x20,0x0a}, {0x21,0x2a},
        {0x40,0x43}, {0x41,0x70}, {0x42,0x70}, {0x07,0x20}, {0x02,0xc1},
        {0x43,0x14}, {0x44,0x14}, {0x45,0x14}, {0x46,0x14},
        {0x4b,0x00}, {0x4c,0x00}, {0x01,0x20}, {0x12,0x02}, {0x11,0x60},
        {0x06,0x00}, {0x06,0x00}, {0x47,0x08}, {0x48,0x08}, {0x49,0x08}, {0x4a,0x08},
        {0x40,0x43}, {0x00,0x71}, {0x00,0x41}
    };
    for (unsigned i = 0; i < sizeof setup / sizeof setup[0]; i++)
        if (!reg(setup[i][0],setup[i][1])) return false;
    return hal_i2c_sys_update_reg(ADDR,0x08,0x01,0) &&
           hal_i2c_sys_update_reg(ADDR,0x14,0x03,0) &&
           hal_i2c_sys_update_reg(ADDR,0x15,0x03,0);
}
bool mic_codec_stop(void)
{
    /* Try every power-down write even after a NACK. */
    bool ok = true;
    for (unsigned r = 0x47; r <= 0x4c; r++) if (!reg(r,0xff)) ok = false;
    if (!reg(0x40,0xc0)) ok = false;
    if (!reg(0x01,0x7f)) ok = false;
    if (!reg(0x06,0x07)) ok = false;
    return ok;
}
