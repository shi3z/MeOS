#include <m5os/camera.h>

bool camera_init(void) { return false; }
bool camera_start(void) { return false; }
void camera_stop(void) { }
const uint16_t *camera_frame(uint32_t *seq_out) { if (seq_out) *seq_out = 0; return NULL; }
bool camera_set_mirror(bool mirror, bool vflip) { (void)mirror; (void)vflip; return false; }
bool camera_set_exposure(uint32_t exp_lines) { (void)exp_lines; return false; }
bool camera_set_gain_index(uint32_t idx) { (void)idx; return false; }
void camera_set_auto_exposure(bool enable) { (void)enable; }
void camera_auto_exposure_step(const uint16_t *frame) { (void)frame; }
void camera_get_status(camera_status_t *out)
{
    if (!out) return;
    out->initialized = false;
    out->streaming   = false;
    out->chip_id     = 0;
    out->frames      = 0;
    out->exposure    = 0;
    out->gain_idx    = 0;
    out->mirror      = false;
    out->vflip       = false;
    out->auto_exp    = false;
}
