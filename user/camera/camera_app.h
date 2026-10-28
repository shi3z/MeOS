#pragma once
#include <m5os/types.h>

#define CAM_PHOTO_W  640
#define CAM_PHOTO_H  360

/* Launch the interactive full-screen camera application.
 * Captured BMP images are saved into `save_dir` (defaults to "/home" if NULL or "/").
 * If `last_saved_name` is non-NULL (size >= 24), receives the basename of the last saved photo. */
void camera_app_run(const char *save_dir, char *last_saved_name);

/* Used by shell `snap`.  (Loading and viewing images is user/viewer/imgview.) */
bool camera_capture_bmp_file(const char *path);
