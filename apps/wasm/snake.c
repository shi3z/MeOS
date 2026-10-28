/* Snake.  Arrows or WASD to steer, Q to quit, Space to restart.  Draws only what changed each step. */
#include "common.h"

#define COLS 40
#define ROWS 24
#define CELL 24
#define OX ((1280 - COLS * CELL) / 2)
#define OY 110
#define MAXLEN (COLS * ROWS)

static unsigned char sx[MAXLEN], sy[MAXLEN];
static int len, dir, ndir, fx, fy;
static unsigned score;

static void cell(int x, int y, unsigned color) { meos_display_fill(OX + x * CELL + 1, OY + y * CELL + 1, CELL - 2, CELL - 2, color); }

static void new_food(void)
{
    for (;;) {
        fx = (int)(meos_rand() % COLS); fy = (int)(meos_rand() % ROWS);
        int hit = 0;
        for (int i = 0; i < len; i++) if (sx[i] == fx && sy[i] == fy) hit = 1;
        if (!hit) break;
    }
    cell(fx, fy, MEOS_RGB(230, 60, 60));
}

static void start(void)
{
    meos_display_fill(0, 0, 1280, 720, 0);
    meos_display_fill(OX - 4, OY - 4, COLS * CELL + 8, ROWS * CELL + 8, MEOS_RGB(200, 200, 200));
    meos_display_fill(OX, OY, COLS * CELL, ROWS * CELL, 0);
    len = 4; dir = ndir = 1; score = 0;
    for (int i = 0; i < len; i++) { sx[i] = (unsigned char)(10 - i); sy[i] = ROWS / 2; cell(sx[i], sy[i], i ? MEOS_RGB(40, 170, 60) : MEOS_RGB(140, 255, 140)); }
    meos_display_text(OX, 20, "SNAKE  arrows/WASD  Q:quit", 26, MEOS_RGB(255, 255, 255), 0, 2);
    new_food();
    hud(OX, 60, "SCORE ", score, MEOS_RGB(255, 220, 80));
}

void _start(void)
{
    if (meos_display_open() < 0) { meos_puts("snake needs the display permission\n"); return; }
    meos_seed_ ^= meos_uptime_ms();
    start();
    int alive = 1;
    unsigned last = meos_uptime_ms(), step_ms = 140;
    for (;;) {
        int k;
        while ((k = meos_getkey()) >= 0) {
            if (k == 'q' || k == 'Q') { meos_display_close(); return; }
            if (!alive && k == ' ') { start(); alive = 1; step_ms = 140; }
            int want = (k == MEOS_KEY_RIGHT || k == 'd') ? 1 : (k == MEOS_KEY_DOWN || k == 's') ? 2 : (k == MEOS_KEY_LEFT || k == 'a') ? 3 : (k == MEOS_KEY_UP || k == 'w') ? 0 : -1;
            if (want >= 0 && want != (dir + 2) % 4) ndir = want;          /* no U-turns */
        }
        if (alive) {
            dir = ndir;
            int nx = sx[0] + (dir == 1) - (dir == 3), ny = sy[0] + (dir == 2) - (dir == 0);
            int eat = nx == fx && ny == fy;
            if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) alive = 0;
            for (int i = 0; alive && i < len - (eat ? 0 : 1); i++) if (sx[i] == nx && sy[i] == ny) alive = 0;
            if (alive) {
                if (!eat) cell(sx[len - 1], sy[len - 1], 0);                  /* the tail leaves its cell */
                else { len++; score += 10; hud(OX, 60, "SCORE ", score, MEOS_RGB(255, 220, 80)); if (step_ms > 60) step_ms -= 4; }
                for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
                cell(sx[0], sy[0], MEOS_RGB(40, 170, 60));                    /* old head becomes body */
                sx[0] = (unsigned char)nx; sy[0] = (unsigned char)ny;
                cell(nx, ny, MEOS_RGB(140, 255, 140));
                if (eat) new_food();
            } else {
                meos_display_text(OX + 280, OY + 270, " GAME OVER - Space ", 19, MEOS_RGB(255, 255, 255), MEOS_RGB(200, 30, 30), 3);
            }
        }
        frame_wait(&last, step_ms);
    }
}
