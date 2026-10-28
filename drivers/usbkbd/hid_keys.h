#pragma once
#include <m5os/types.h>

/* HID keyboard usage IDs (USB HID Usage Tables, "Keyboard/Keypad" page), US layout, to console bytes. */
#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20
#define HID_MOD_SHIFT   (HID_MOD_LSHIFT | HID_MOD_RSHIFT)
#define HID_USAGE_FIRST 0x04          /* 'a'; 0..3 mean "no key" / rollover error */
#define HID_USAGE_CAPSLOCK 0x39

/* Fills out[] with the bytes the console should receive (NUL-terminated; empty if the key prints nothing). */
void hid_decode(uint8_t usage, uint8_t mods, bool caps_lock, char out[8]);
