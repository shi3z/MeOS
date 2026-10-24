#include "replay.h"
#include <string.h>

bool sim_shot_name(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= sizeof((sim_action *)0)->shot)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}
static bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool number(const char *s, unsigned limit, unsigned *out)
{
    unsigned value = 0;
    if (!*s)
        return false;
    while (*s) {
        if (*s < '0' || *s > '9')
            return false;
        unsigned digit = (unsigned)(*s++ - '0');
        if (digit > limit || value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}
bool sim_replay_load(FILE *file, sim_replay *out, char *error, size_t capacity)
{
    memset(out, 0, sizeof *out);
    unsigned line_number = 0;
    char line[256];
    const char *reason = NULL;
    while (fgets(line, sizeof line, file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file)) {
            reason = "line too long";
            break;
        }
        char *comment = strchr(line, '#');
        if (comment)
            *comment = 0;
        char *tokens[6];
        unsigned count = 0;
        for (char *p = line; *p;) {
            while (whitespace(*p))
                p++;
            if (!*p)
                break;
            if (count == 6) {
                reason = "too many fields";
                break;
            }
            tokens[count++] = p;
            while (*p && !whitespace(*p))
                p++;
            if (*p)
                *p++ = 0;
        }
        if (reason)
            break;
        if (!count)
            continue;
        if (out->count == SIM_ACTION_MAX) {
            reason = "too many actions";
            break;
        }
        sim_action action = {0};
        unsigned delay, usage, modifiers, caps;
        if (!strcmp(tokens[0], "key")) {
            if (count != 5 || !number(tokens[1], 60000, &delay) ||
                !number(tokens[2], 255, &usage) || usage < 4 ||
                !number(tokens[3], 255, &modifiers) || !number(tokens[4], 1, &caps)) {
                reason = "expected key DELAY_MS HID_USAGE MODIFIERS CAPS";
                break;
            }
            action.usage = (uint8_t)usage;
            action.modifiers = (uint8_t)modifiers;
            action.caps = (uint8_t)caps;
        } else if (!strcmp(tokens[0], "shot")) {
            if (count != 3 || !number(tokens[1], 60000, &delay) || !sim_shot_name(tokens[2])) {
                reason = "expected shot DELAY_MS SAFE_NAME";
                break;
            }
            for (size_t i = 0; i < out->count; i++)
                if (!strcmp(out->actions[i].shot, tokens[2])) {
                    reason = "duplicate screenshot name";
                    break;
                }
            if (reason)
                break;
            strcpy(action.shot, tokens[2]);
        } else {
            reason = "unknown action";
            break;
        }
        if (delay > SIM_SCRIPT_MAX_MS - out->duration) {
            reason = "script duration limit";
            break;
        }
        action.delay = delay;
        out->duration += delay;
        out->actions[out->count++] = action;
    }
    if (!reason && ferror(file))
        reason = "read error";
    if (!reason && !out->count)
        reason = "empty script";
    if (reason) {
        snprintf(error, capacity, "replay line %u: %s", line_number, reason);
        out->count = 0;
        return false;
    }
    return true;
}
