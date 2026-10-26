#pragma once
#include <m5os/types.h>

typedef enum {
    HOME_NONE = 0,
    HOME_WRITE,
    HOME_SCHEME,
    HOME_FILES,
    HOME_CAMERA,
    HOME_WEB,
    HOME_WIFI,
    HOME_SSH,
    HOME_TAILSCALE,
    HOME_APPS,
    HOME_BASIC,
    HOME_DOOM,
    HOME_QUAKE,
    HOME_SHELL,
} home_action_t;

typedef bool (*home_touch_reader_t)(int *x, int *y, void *context);

int home_item_count(void);
home_action_t home_item_action(int index);
const char *home_item_label(int index);
const char *home_item_detail(int index);

/* Draw the Tab5 home screen and wait for keyboard or touch selection.
 * `selected` is both the initial and returned cursor position. */
home_action_t home_select(int *selected, home_touch_reader_t touch, void *context);

