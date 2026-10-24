#pragma once
#include <m5os/types.h>
/* Cooperative native keyboard lease. Existing text/IME path remains the default.
 * No app pointers/callbacks escape: the owner is only an opaque identity token. */
typedef struct { uint8_t usage; bool down; } keyboard_raw_event;
#define KEYBOARD_RAW_EVENTS 64
typedef struct { uint8_t keys[32],mods; bool ready;
    uint8_t count; keyboard_raw_event events[KEYBOARD_RAW_EVENTS];
} keyboard_raw_state;
bool keyboard_raw_claim(const void *owner);
void keyboard_raw_release(const void *owner);
bool keyboard_raw_active(void);
bool keyboard_raw_read(const void *owner,keyboard_raw_state *out);
/* Keyboard-driver producers. Errors/overflow release keys, never leave a hold. */
void keyboard_raw_clear(void);
uint32_t keyboard_raw_generation(void);
void keyboard_raw_ready(uint32_t epoch);
void keyboard_raw_matrix(uint32_t epoch,uint8_t event);
void keyboard_raw_usb(uint32_t epoch,const uint8_t report[8]);
