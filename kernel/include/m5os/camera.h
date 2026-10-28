#pragma once
#include <m5os/types.h>

#define CAMERA_WIDTH   1280
#define CAMERA_HEIGHT  720

typedef struct {
    bool     initialized;
    bool     streaming;
    uint16_t chip_id;
    uint32_t frames;
    uint32_t exposure;
    uint32_t gain_idx;
    bool     mirror;
    bool     vflip;
    bool     auto_exp;
} camera_status_t;

bool camera_init(void);
bool camera_start(void);
void camera_stop(void);

/* Returns a pointer to the latest completed 1280x720 RGB565 frame in PSRAM (or NULL if none yet). */
const uint16_t *camera_frame(uint32_t *seq_out);

bool camera_set_mirror(bool mirror, bool vflip);
bool camera_set_exposure(uint32_t exp_lines);
bool camera_set_gain_index(uint32_t idx);
void camera_set_auto_exposure(bool enable);
void camera_auto_exposure_step(const uint16_t *frame);
void camera_get_status(camera_status_t *out);
