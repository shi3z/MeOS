#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/tab5kbd/tab5touch.h"
#include "../../apps/doom/runtime.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "../../apps/doom/layout.h"
static uint32_t now;
static tab5_touch_point_t points[10];
static unsigned count, pos;
static const char *text = "";
static bool down[256];
static jmp_buf quit;
uint32_t DG_GetTicksMs(void) { return now; }
uint64_t hal_time_now_us(void) { return (uint64_t)now*1000; }
void doom_profile_input(uint32_t us) { (void)us; }
int kconsole_getc(void) { return text[pos] ? (unsigned char)text[pos++] : -1; }
unsigned tab5_touch_read_points(tab5_touch_point_t *p, unsigned cap)
{ assert(cap >= count); memcpy(p, points, count * sizeof *p); return count; }
void doom_exit(int code) { assert(code == 0); longjmp(quit, 1); }
static unsigned drain(void)
{
    unsigned n = 0; int pressed; unsigned char key;
    while (DG_GetKey(&pressed, &key)) {
        assert(++n < 512); assert(down[key] != !!pressed); down[key] = !!pressed;
    }
    return n;
}
static void finger(unsigned i, unsigned button)
{ const doom_button_t *b=&doom_buttons[button];
  points[i] = (tab5_touch_point_t){ .x=b->x+b->w/2, .y=b->y+b->h/2, .id=i }; }
int main(void)
{
    for(unsigned i=0;i<DOOM_BUTTON_COUNT;i++) {
        const doom_button_t *b=&doom_buttons[i];
        assert(b->x>=0 && b->y>=0 && b->x+b->w<=1280 && b->y+b->h<=720);
        assert(b->x+b->w<=DOOM_VIEW_X || b->x>=DOOM_VIEW_X+DOOM_VIEW_W);
        assert(doom_button_at(b->x+b->w/2,b->y+b->h/2)==(int)i);
        assert(doom_button_at(b->x+b->w,b->y+b->h/2)!=(int)i);
        assert(doom_button_at(b->x+b->w/2,b->y+b->h)!=(int)i);
    }
    doom_input_reset(); now = 20;
    finger(0, 2); finger(1, 4); count = 2;
    assert(drain() == 2 && down[KEY_UPARROW] && down[KEY_FIRE]);
    now += 20; assert(drain() == 0); /* held frames never retrigger */
    points[0] = points[1]; count = 1; now += 20;
    assert(drain() == 1 && !down[KEY_UPARROW] && down[KEY_FIRE]);
    finger(1, 4); count = 2; now += 20; assert(!drain()); /* two fingers on fire */
    count = 1; now += 20; assert(!drain() && down[KEY_FIRE]);
    finger(0, 5); now += 20;
    assert(drain() == 2 && !down[KEY_FIRE] && down[KEY_USE]); /* slide */
    count = 0; now += 20; assert(drain() == 1 && !down[KEY_USE]);
    points[0]=(tab5_touch_point_t){ .x=640,.y=650 }; count=1;now+=20;
    assert(!drain()); /* bottom of game view must not hit old toolbar */
    points[0]=(tab5_touch_point_t){ .x=80,.y=500 };now+=20;
    assert(!drain()); /* gap between turn buttons */
    count=0;
    /* Keyboard expiry must not release a key held by touch. */
    finger(0, 4); count = 1; text = " "; pos = 0; now += 20;
    assert(drain() == 1 && down[KEY_FIRE]);
    now += 200; assert(!drain() && down[KEY_FIRE]);
    count = 0; now += 20; assert(drain() == 1 && !down[KEY_FIRE]);
    finger(0, 4); count = 1; text = " "; pos = 0; now += 20; assert(drain() == 1);
    count = 0; now += 20; assert(!drain() && down[KEY_FIRE]);
    now += 200; assert(drain() == 1 && !down[KEY_FIRE]);
    text = "\033[Ae"; pos = 0; now += 20;
    assert(drain() == 2 && down[KEY_UPARROW] && down[KEY_USE]);
    now += 200; assert(drain() == 2);
    text = ""; pos = 0; finger(0, 8); count = 1; now += 20;
    if (!setjmp(quit)) { drain(); assert(!"QUIT must exit"); }
    count = 0; doom_input_reset(); memset(down, 0, sizeof down); now += 20;
    assert(!drain()); /* re-entry clears held state */
    puts("Doom input: multi-touch, independent release, slide, duplicate contacts, keyboard merge, quit PASS");
}
