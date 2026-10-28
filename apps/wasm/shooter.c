/* Vertical scrolling shooter.  Left/Right (or A/D) move, the ship fires by itself, Q quits, Space restarts. */
#include "common.h"

#define PX 400            /* playfield */
#define PY 40
#define PW 480
#define PH 640
#define MAXB 16
#define MAXE 14
#define MAXS 40

typedef struct { int x, y, ox, oy, on; } obj_t;
static obj_t bul[MAXB], en[MAXE];
static struct { int x, y, ox, oy; } star[MAXS];
static int px, opx, lives;
static unsigned score;
static unsigned short ship[256], foe[256];

static const char *SHIP[16] = {
    "......G..G......", "......G..G......", ".....GGGGGG.....", ".....GBBBBG.....", "....GGBBBBGG....", "....GGGBBGGG....", "...GGGGBBGGGG...", "..GGGGGBBGGGGG..",
    ".GGGGGGGGGGGGGG.", "GGGGGGGGGGGGGGGG", "GG.GGGGGGGGGG.GG", "G..GG.RRRR.GG..G", "...GG.RRRR.GG...", ".....RRRRRR.....", "......RRRR......", ".......RR.......",
};
static const char *FOE[16] = {
    "................", "..RR........RR..", ".RRRR......RRRR.", ".RRRRRRRRRRRRRR.", "RRRRRRRRRRRRRRRR", "RRYYRRRRRRRRYYRR", "RRYYRRRRRRRRYYRR", "RRRRRRRRRRRRRRRR",
    ".RRRRRRRRRRRRRR.", "..RRRR.RR.RRRR..", "...RR..RR..RR...", "...RR..RR..RR...", "....R..RR..R....", ".....R.RR.R.....", "................", "................",
};

static void make_sprite(unsigned short *out, const char **m)
{
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        char c = m[y][x];
        out[y * 16 + x] = c == 'G' ? MEOS_RGB(90, 200, 255) : c == 'B' ? MEOS_RGB(255, 255, 255) : c == 'R' ? MEOS_RGB(230, 70, 50) : c == 'Y' ? MEOS_RGB(255, 230, 60) : 0;
    }
}

static int hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) { return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah; }

static void reset(void)
{
    for (int i = 0; i < MAXB; i++) bul[i].on = 0;
    for (int i = 0; i < MAXE; i++) en[i].on = 0;
    px = opx = PW / 2 - 16; lives = 3; score = 0;
    meos_display_fill(0, 0, 1280, 720, 0);
    meos_display_fill(PX - 4, PY - 4, PW + 8, PH + 8, MEOS_RGB(90, 90, 120));
    meos_display_fill(PX, PY, PW, PH, MEOS_RGB(0, 0, 20));
    meos_display_text(20, 20, "SHOOTER", 7, MEOS_RGB(255, 255, 255), 0, 2);
    meos_display_text(20, 56, "<- -> / A D  move", 17, MEOS_RGB(160, 160, 160), 0, 1);
    meos_display_text(20, 72, "auto fire  Q quit", 17, MEOS_RGB(160, 160, 160), 0, 1);
}

static void draw_status(void)
{
    hud(20, 120, "SCORE ", score, MEOS_RGB(255, 220, 80));
    hud(20, 160, "LIVES ", (unsigned)lives, MEOS_RGB(120, 255, 120));
}

void _start(void)
{
    if (meos_display_open() < 0) { meos_puts("shooter needs the display permission\n"); return; }
    make_sprite(ship, SHIP); make_sprite(foe, FOE);
    meos_seed_ ^= meos_uptime_ms();
    for (int i = 0; i < MAXS; i++) { star[i].x = (int)(meos_rand() % PW); star[i].y = (int)(meos_rand() % PH); star[i].ox = star[i].x; star[i].oy = star[i].y; }
    reset();
    int alive = 1, invuln = 0;
    unsigned last = meos_uptime_ms(), frame = 0;
    for (;;) {
        int k, dx = 0;
        while ((k = meos_getkey()) >= 0) {
            if (k == 'q' || k == 'Q') { meos_display_close(); return; }
            if (k == MEOS_KEY_LEFT || k == 'a') dx = -1;
            if (k == MEOS_KEY_RIGHT || k == 'd') dx = 1;
            if (!alive && k == ' ') { reset(); alive = 1; invuln = 0; }
        }
        if (alive) {
            frame++;
            px += dx * 18;
            if (px < 0) px = 0;
            if (px > PW - 32) px = PW - 32;
            if (frame % 6 == 0) for (int i = 0; i < MAXB; i++) if (!bul[i].on) { bul[i] = (obj_t){ px + 14, PH - 70, 0, 0, 1 }; break; }
            if (frame % (20 - (int)(score / 400 < 12 ? score / 400 : 12)) == 0)
                for (int i = 0; i < MAXE; i++) if (!en[i].on) { en[i] = (obj_t){ (int)(meos_rand() % (PW - 32)), -32, 0, 0, 1 }; break; }
            for (int i = 0; i < MAXB; i++) if (bul[i].on) { bul[i].y -= 16; if (bul[i].y < -10) bul[i].on = 0; }
            for (int i = 0; i < MAXE; i++) if (en[i].on) {
                en[i].y += 4 + (int)(score / 600);
                if (en[i].y > PH) en[i].on = 0;
                for (int j = 0; j < MAXB; j++)
                    if (bul[j].on && hit(bul[j].x, bul[j].y, 4, 12, en[i].x, en[i].y, 32, 32)) { bul[j].on = 0; en[i].on = 0; score += 20; }
                if (en[i].on && !invuln && hit(px + 4, PH - 56, 24, 24, en[i].x + 4, en[i].y + 4, 24, 24)) {
                    en[i].on = 0; lives--; invuln = 45;
                    if (lives <= 0) alive = 0;
                }
            }
            if (invuln) invuln--;
            for (int i = 0; i < MAXS; i++) { star[i].ox = star[i].x; star[i].oy = star[i].y; star[i].y += 2 + i % 3; if (star[i].y >= PH) { star[i].y = 0; star[i].x = (int)(meos_rand() % PW); } }
        }
        /* draw: erase everything that was drawn last frame, then draw the new positions */
        for (int i = 0; i < MAXS; i++) {
            if (star[i].oy >= 0 && star[i].oy < PH) meos_display_fill(PX + star[i].ox, PY + star[i].oy, 2, 3, MEOS_RGB(0, 0, 20));
        }
        for (int i = 0; i < MAXB; i++) if (bul[i].ox || bul[i].oy) { meos_display_fill(PX + bul[i].ox, PY + bul[i].oy, 4, 12, MEOS_RGB(0, 0, 20)); bul[i].ox = bul[i].oy = 0; }
        for (int i = 0; i < MAXE; i++) if (en[i].ox || en[i].oy) { meos_display_fill(PX + en[i].ox, PY + en[i].oy, 32, 32, MEOS_RGB(0, 0, 20)); en[i].ox = en[i].oy = 0; }
        meos_display_fill(PX + opx, PY + PH - 64, 32, 32, MEOS_RGB(0, 0, 20));
        for (int i = 0; i < MAXS; i++) meos_display_fill(PX + star[i].x, PY + star[i].y, 2, 3, i % 3 ? MEOS_RGB(120, 120, 160) : MEOS_RGB(220, 220, 255));
        for (int i = 0; i < MAXB; i++) if (bul[i].on) {
            meos_display_fill(PX + bul[i].x, PY + bul[i].y, 4, 12, MEOS_RGB(255, 240, 120));
            bul[i].ox = bul[i].x; bul[i].oy = bul[i].y ? bul[i].y : 1;
        }
        for (int i = 0; i < MAXE; i++) if (en[i].on && en[i].y > -32) {
            int y = en[i].y < 0 ? 0 : en[i].y;                              /* sprites are drawn from y >= 0 */
            if (en[i].y >= 0) meos_display_blit(PX + en[i].x, PY + y, 16, 16, foe, 2);
            en[i].ox = en[i].x; en[i].oy = y ? y : 1;
        }
        if (alive && (invuln == 0 || (frame & 2))) meos_display_blit(PX + px, PY + PH - 64, 16, 16, ship, 2);
        opx = px;
        draw_status();
        if (!alive) meos_display_text(PX + 100, PY + 280, "  GAME OVER - Space  ", 21, MEOS_RGB(255, 255, 255), MEOS_RGB(200, 30, 30), 2);
        frame_wait(&last, 33);
    }
}
