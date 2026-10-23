/* INA226 register/scaling reference: https://www.ti.com/lit/ds/symlink/ina226.pdf
 * Tab5 shunt = 5 mOhm; negative shunt current means charge, as documented in
 * https://github.com/m5stack/M5Unified/blob/master/src/utility/Power_Class.inl
 * Use raw shunt voltage, avoiding dependence on the writable calibration register.
 * Integer math keeps the S3 no-floating-point task convention intact too. */
#include <m5os/power.h>
#include <m5os/board_config.h>
#include <m5os/hal_i2c.h>

const char *power_result_name(power_result_t r)
{
    switch (r) {
    case POWER_OK: return "ready";
    case POWER_UNSUPPORTED: return "unsupported";
    case POWER_NO_RESPONSE: return "no response";
    case POWER_WRONG_DEVICE: return "unexpected identity";
    case POWER_READ_ERROR: return "read error";
    case POWER_NOT_READY: return "conversion unavailable";
    default: return "unknown";
    }
}

#if M5OS_BOARD_TAB5
static bool read_word(uint8_t reg, uint16_t *value)
{
    uint8_t b[2];
    if (!hal_i2c_sys_read_regs(0x41, reg, b, sizeof b)) return false;
    *value = ((uint16_t)b[0] << 8) | b[1];
    return true;
}
#endif

void power_read(power_sample_t *out)
{
    *out = (power_sample_t){ .result = POWER_UNSUPPORTED, .estimated_percent = -1 };
#if M5OS_BOARD_TAB5
    out->address = 0x41;
    uint8_t direction, latch, input;
    if (hal_i2c_sys_read_regs(0x44, 0x03, &direction, 1) &&
        hal_i2c_sys_read_regs(0x44, 0x05, &latch, 1) &&
        hal_i2c_sys_read_regs(0x44, 0x0f, &input, 1) &&
        (direction & 0x80) && !(direction & 0x40)) {
        out->charger_valid = true;
        out->charge_enabled = (latch & 0x80) != 0;
        out->charge_status_high = (input & 0x40) != 0;
    }
    out->result = POWER_NO_RESPONSE;
    if (!read_word(0xfe, &out->manufacturer)) return;
    out->result = POWER_READ_ERROR;
    if (!read_word(0xff, &out->die)) return;
    if (out->manufacturer != 0x5449 || (out->die & 0xfff0) != 0x2260) {
        out->result = POWER_WRONG_DEVICE; return;
    }
    uint16_t bus, shunt;
    if (!read_word(0x00, &out->config)) return;
    /* Require continuous shunt+bus conversion. Other modes can expose stale values.
     * Default reset configuration is 0x4127. Do not reconfigure an externally set monitor. */
    if ((out->config & 7) != 7) { out->result = POWER_NOT_READY; return; }
    if (!read_word(0x02, &bus) || !read_word(0x01, &shunt)) return;
    if (bus & 0x8000) { out->result = POWER_READ_ERROR; return; } /* reserved bit */
    int32_t signed_shunt = (shunt & 0x8000) ? (int32_t)shunt - 65536 : shunt;
    out->battery_mv = ((int32_t)bus * 5 + 2) / 4;  /* 1.25 mV per LSB */
    out->shunt_nv = signed_shunt * 2500;           /* 2.5 uV per LSB */
    out->battery_ua = -signed_shunt * 500;         /* 2.5 uV / 5 mOhm = 0.5 mA */
    /* Coarse 2S voltage estimate; suppress it outside a plausible pack range.
     * This is neither a fuel gauge nor proof a battery is installed. */
    if (out->battery_mv >= 6000 && out->battery_mv <= 8600) {
        int level = (out->battery_mv - 6600) * 100 / 1700;
        out->estimated_percent = level < 0 ? 0 : level > 100 ? 100 : level;
    }
    out->result = POWER_OK;
#endif
}
