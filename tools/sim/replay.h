#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define SIM_ACTION_MAX 4096
#define SIM_SCRIPT_MAX_MS 300000
typedef struct {
    uint32_t delay;
    uint8_t usage, modifiers, caps;
    char shot[48]; /* nonempty for screenshot actions */
} sim_action;
typedef struct {
    sim_action actions[SIM_ACTION_MAX];
    size_t count;
    uint32_t duration;
} sim_replay;
bool sim_replay_load(FILE *, sim_replay *, char *error, size_t error_capacity);
bool sim_shot_name(const char *);
