#pragma once
#include <m5os/types.h>

typedef enum {
    POWER_UNSUPPORTED, POWER_NO_RESPONSE, POWER_WRONG_DEVICE,
    POWER_READ_ERROR, POWER_NOT_READY, POWER_OK
} power_result_t;

typedef struct {
    power_result_t result;
    uint8_t address;
    uint16_t manufacturer, die, config;
    int32_t battery_mv;
    int32_t battery_ua;          /* positive = into battery, negative = out (Tab5 5 mOhm shunt) */
    int32_t shunt_nv;            /* raw INA226 polarity, before the battery-current inversion */
    int estimated_percent;      /* coarse voltage-only 2S estimate; -1 = unavailable */
    bool charger_valid;
    bool charge_enabled;        /* configuration, NOT proof of actual charging */
    bool charge_status_high;    /* raw expander CHG_STAT input; NOT a power-source detector */
} power_sample_t;

/* Read-only, synchronous sample. Never changes charger, monitor configuration or calibration.
 * No automatic source inference: USB / external DC cannot be distinguished by this sensor. */
void power_read(power_sample_t *out);
const char *power_result_name(power_result_t result);
