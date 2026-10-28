/* Tetris.  Left/Right move, Up or X rotate, Z rotate back, Down soft drop, Space hard drop, P pause, Q quit. */
#include "common.h"

#define W 10
#define H 20
#define CELL 32
#define OX 480
#define OY 40

/* four rotations of each piece as 4x4 bitmaps (bit 15 = top-left) */
static const unsigned short SHAPE[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },    /* I */
    { 0xCC00, 0xCC00, 0xCC00, 0xCC00 },    /* O */
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 },    /* T */
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 },    /* S */
    { 0xC600, 0x2640, 0x0C60, 0x4C80 },    /* Z */
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 },    /* J */
    { 0x2E00, 0x4460, 0x0E80, 0xC440 },    /* L */
};
static unsigned COLOR[8];

static unsigned char board[H][W];          /* 0 empty, 1..7 piece colour */
static int px, py, rot, cur, nxt;
static unsigned score, lines, level;

static int fits(int x, int y, int r, int p)
{
    unsigned short m = SHAPE[p][r];
    for (int i = 0; i < 16; i++) if (m & (0x8000 >> i)) {
        int cx = x + i % 4, cy = y + i / 4;
        if (cx < 0 || cx >= W || cy >= H) return 0;
        if (cy >= 0 && board[cy][cx]) return 0;
    }
    return 1;
}

static void spawn(void)
{
    cur = nxt; nxt = (int)(meos_rand() % 7);
    rot = 0; px = 3; py = -1;
}

static void cell(int bx, int by, unsigned c)
{
    meos_display_fill(OX + bx * CELL + 1, OY + by * CELL + 1, CELL - 2, CELL - 2, c);
}

static void draw_all(int over, int paused)
{
    meos_display_fill(OX - 6, OY - 6, W * CELL + 12, H * CELL + 12, MEOS_RGB(110, 110, 140));
    meos_display_fill(OX, OY, W * CELL, H * CELL, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (board[y][x]) cell(x, y, COLOR[board[y][x]]);
    /* ghost: where the piece would land */
    int gy = py;
    while (fits(px, gy + 1, rot, cur)) gy++;
    unsigned short m = SHAPE[cur][rot];
    for (int i = 0; i < 16; i++) if (m & (0x8000 >> i)) {
        int cx = px + i % 4, cy = gy + i / 4;
        if (cy >= 0) meos_display_fill(OX + cx * CELL + 1, OY + cy * CELL + 1, CELL - 2, CELL - 2, MEOS_RGB(50, 50, 70));
    }
    for (int i = 0; i < 16; i++) if (m & (0x8000 >> i)) {
        int cx = px + i % 4, cy = py + i / 4;
        if (cy >= 0) cell(cx, cy, COLOR[cur + 1]);
    }
    /* side panel */
    meos_display_fill(OX + W * CELL + 40, OY, 200, 150, 0);
    meos_display_text(OX + W * CELL + 40, OY, "NEXT", 4, MEOS_RGB(255, 255, 255), 0, 2);
    unsigned short nm = SHAPE[nxt][0];
    for (int i = 0; i < 16; i++) if (nm & (0x8000 >> i))
        meos_display_fill(OX + W * CELL + 40 + (i % 4) * 24, OY + 40 + (i / 4) * 24, 22, 22, COLOR[nxt + 1]);
    hud(120, OY + 20, "SCORE ", score, MEOS_RGB(255, 220, 80));
    hud(120, OY + 60, "LINES ", lines, MEOS_RGB(120, 255, 120));
    hud(120, OY + 100, "LEVEL ", level, MEOS_RGB(120, 200, 255));
    meos_display_text(100, OY + 240, "< > move  ^ rotate", 18, MEOS_RGB(160, 160, 160), 0, 1);
    meos_display_text(100, OY + 256, "v soft  Space drop", 18, MEOS_RGB(160, 160, 160), 0, 1);
    meos_display_text(100, OY + 272, "P pause   Q quit", 16, MEOS_RGB(160, 160, 160), 0, 1);
    if (over) meos_display_text(OX + 30, OY + 280, " GAME OVER - Space ", 19, MEOS_RGB(255, 255, 255), MEOS_RGB(200, 30, 30), 2);
    if (paused) meos_display_text(OX + 80, OY + 280, "  PAUSED  ", 10, MEOS_RGB(0, 0, 0), MEOS_RGB(255, 220, 80), 2);
}

static void lock_piece(void)
{
    unsigned short m = SHAPE[cur][rot];
    for (int i = 0; i < 16; i++) if (m & (0x8000 >> i)) {
        int cx = px + i % 4, cy = py + i / 4;
        if (cy >= 0) board[cy][cx] = (unsigned char)(cur + 1);
    }
    int cleared = 0;
    for (int y = H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < W; x++) if (!board[y][x]) full = 0;
        if (!full) continue;
        for (int yy = y; yy > 0; yy--) for (int x = 0; x < W; x++) board[yy][x] = board[yy - 1][x];
        for (int x = 0; x < W; x++) board[0][x] = 0;
        cleared++; y++;                                        /* look at the same row again */
    }
    static const unsigned PTS[5] = { 0, 100, 300, 500, 800 };
    score += PTS[cleared] * (level + 1);
    lines += (unsigned)cleared;
    level = lines / 10;
}

static void start(void)
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) board[y][x] = 0;
    score = lines = level = 0;
    nxt = (int)(meos_rand() % 7);
    spawn();
    meos_display_fill(0, 0, 1280, 720, 0);
}

void _start(void)
{
    if (meos_display_open() < 0) { meos_puts("tetris needs the display permission\n"); return; }
    COLOR[1] = MEOS_RGB(0, 220, 230); COLOR[2] = MEOS_RGB(240, 220, 0); COLOR[3] = MEOS_RGB(170, 60, 220); COLOR[4] = MEOS_RGB(60, 210, 60);
    COLOR[5] = MEOS_RGB(230, 50, 50); COLOR[6] = MEOS_RGB(60, 80, 230); COLOR[7] = MEOS_RGB(240, 140, 20);
    meos_seed_ ^= meos_uptime_ms();
    start();
    int over = 0, paused = 0, dirty = 1;
    unsigned last = meos_uptime_ms(), fall = 0;
    for (;;) {
        int k;
        while ((k = meos_getkey()) >= 0) {
            if (k == 'q' || k == 'Q') { meos_display_close(); return; }
            if (over) { if (k == ' ') { start(); over = 0; dirty = 1; } continue; }
            if (k == 'p' || k == 'P') { paused = !paused; dirty = 1; continue; }
            if (paused) continue;
            if ((k == MEOS_KEY_LEFT || k == 'a') && fits(px - 1, py, rot, cur)) { px--; dirty = 1; }
            else if ((k == MEOS_KEY_RIGHT || k == 'd') && fits(px + 1, py, rot, cur)) { px++; dirty = 1; }
            else if (k == MEOS_KEY_UP || k == 'x' || k == 'X' || k == 'w') {
                int nr = (rot + 1) & 3;
                if (fits(px, py, nr, cur)) { rot = nr; dirty = 1; }
                else if (fits(px - 1, py, nr, cur)) { px--; rot = nr; dirty = 1; }          /* wall kick */
                else if (fits(px + 1, py, nr, cur)) { px++; rot = nr; dirty = 1; }
            } else if (k == 'z' || k == 'Z') {
                int nr = (rot + 3) & 3;
                if (fits(px, py, nr, cur)) { rot = nr; dirty = 1; }
            } else if (k == MEOS_KEY_DOWN || k == 's') {
                if (fits(px, py + 1, rot, cur)) { py++; score++; dirty = 1; fall = 0; }
            } else if (k == ' ') {
                while (fits(px, py + 1, rot, cur)) { py++; score += 2; }
                fall = 1000000;                                                              /* lock now */
                dirty = 1;
            }
        }
        if (!over && !paused) {
            unsigned interval = level >= 12 ? 60 : 700 - level * 55;
            fall += 33;
            if (fall >= interval) {
                fall = 0;
                if (fits(px, py + 1, rot, cur)) py++;
                else {
                    lock_piece();
                    spawn();
                    if (!fits(px, py, rot, cur)) over = 1;
                }
                dirty = 1;
            }
        }
        if (dirty) { draw_all(over, paused); dirty = 0; }
        frame_wait(&last, 33);
    }
}
