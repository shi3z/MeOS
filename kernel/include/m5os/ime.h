#pragma once
#include <m5os/types.h>

/*
 * Japanese input method (FEP, "front-end processor") for the keyboards.
 *
 * It sits between a keyboard driver and the console input: the drivers hand it the bytes of every key
 * (ime_feed) and what comes out of kconsole_getc() is already UTF-8 text.  With the IME off the bytes pass
 * through unchanged.  Toggle with Ctrl+Space (or Alt+`).  While it is on:
 *
 *   letters         romaji -> hiragana (katakana in katakana mode), shown underlined-in-blue at the cursor
 *   Space           convert to kanji (second press: candidate list; Space/Down next, Up previous, 1-9 pick)
 *   Left/Right      move between phrases while converting (in the reading: move the cursor)
 *   Shift+Left/Right  make the phrase shorter / longer
 *   Enter           commit; Esc / Backspace while converting go back to the reading
 *   F6 F7 F9 F10    hiragana / katakana / full-width / half-width Latin
 *
 * Full-screen programs that own the display (fbcon paused) get the same UI as a two-line bar at the bottom of
 * the screen, which is restored when the composition ends.
 *
 * Dictionary: a small one is built in; a large SKK dictionary can be loaded
 * asynchronously from /assets/ime-dict.bin on SD (tools/ime_flash_dict.sh).
 */

#define IME_TOGGLE_KEY  "\x1b[99~"      /* what the key decoders emit for the on/off key */

void ime_init(void);                    /* optional: loads the large dictionary in the background */
void ime_feed(const char *key);         /* the console bytes of one key press, replaces kconsole_inject() */
bool ime_active(void);                  /* on (Japanese input) ? */
const char *ime_status(void);
/* App-owned composition UI for proportional/large-font text editors. The
 * normal IME renderer stays unchanged for other apps. Snapshot copies state;
 * no pointers into the conversion engine escape to the application. */
#define IME_VIEW_CPS 200
#define IME_VIEW_CANDIDATES 9
#define IME_COMMIT_BYTES 4612
typedef struct {
    uint32_t revision;
    bool active, converting, listing;
    int count, candidate_count, selected;
    uint16_t text[IME_VIEW_CPS];
    uint8_t styles[IME_VIEW_CPS];
    char candidates[IME_VIEW_CANDIDATES][192];
    int numbers[IME_VIEW_CANDIDATES];
} ime_view;
void ime_set_app_view(bool enabled);
bool ime_read_view(ime_view *out);
/* App-owned editors receive one complete IME confirmation at a time instead
 * of reconstructing it from the console byte stream.  The returned UTF-8 is
 * not NUL-terminated.  Zero means none is pending or capacity is too small;
 * a too-small buffer leaves the confirmation pending. */
size_t ime_take_commit(char *out, size_t capacity);
/* A full-screen program that edits a line of text says where its insertion point is (in text cells); while it is set
 * the composition is drawn inline there, with the candidate list under it, instead of as a bar at the bottom.
 * Call it before every wait for a key (cheap), and ime_clear_anchor() when the field goes away. */
#if M5OS_BOARD_TAB5
void ime_bypass(bool on);      /* a full-screen app that wants raw keys (games): keys go straight through, composition is cancelled */
void ime_set_anchor(int cx, int cy);
void ime_clear_anchor(void);
#else
static inline void ime_bypass(bool on) { (void)on; }
static inline void ime_set_anchor(int cx, int cy) { (void)cx; (void)cy; }
static inline void ime_clear_anchor(void) {}
#endif           /* "off", "hiragana", "katakana", with the dictionary state; for the shell */
