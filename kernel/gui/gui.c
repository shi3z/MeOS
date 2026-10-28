/*
 * M5OS GUI service: a stack of maximized windows, a status bar, three system buttons,
 * and the translation from touch coordinates to events.  See m5os/gui.h for the model.
 *
 * Locking: s_lock protects the window table AND serializes drawing, so a window call and
 * the GUI task never paint at the same time.  Lock order is s_lock -> SPI bus lock (taken
 * inside display_*); nothing that holds the bus lock ever takes s_lock.
 */
#include <m5os/gui.h>
#include <m5os/display.h>
#include <m5os/app.h>
#include <m5os/task.h>
#include <m5os/mutex.h>
#include <m5os/vfs.h>
#include <m5os/dev_buttons.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include <m5os/errno.h>
#include <m5os/arch.h>

#define WIN_MAX          8
#define TITLE_MAX        16
#define POLL_MS          30u
#define MOVE_THRESHOLD   3          /* pixels before a MOVE is reported */
#define CLOSE_GRACE_MS   1000u      /* after Close, how long an app gets to leave on its own */
#define STATUS_PERIOD_MS 1000u
#define INJECT_DEPTH     8
#define GUID_STACK       6144u
#define GUID_PRIO        (TASK_PRIO_DEFAULT + 2)

#define GUI_MSG_MAGIC    0x47550000u       /* "GU": high half of the IPC type of every GUI event */

#define COL_DESKTOP      COLOR_BLACK
#define COL_STATUS_BG    RGB565(40, 40, 48)
#define COL_STATUS_FG    RGB565(200, 200, 210)
#define COL_TITLE_BG     RGB565(0, 70, 140)
#define COL_TITLE_FG     COLOR_WHITE
#define COL_BTN          RGB565(70, 70, 80)
#define COL_BTN_DOWN     RGB565(40, 160, 70)
#define COL_BTN_FG       COLOR_WHITE

typedef struct {
    bool       used;
    window_t   id;
    uint32_t   owner_pid;
    uint32_t   owner_app;                 /* 0 = a kernel task */
    char       title[TITLE_MAX];
    uint32_t   flags;
    uint32_t   close_deadline_ms;         /* 0 = none */
} win_t;

static win_t   s_win[WIN_MAX];
static int     s_order[WIN_MAX];          /* indices into s_win, back .. front */
static int     s_nord;
static mutex_t s_lock = MUTEX_INIT;
static window_t s_next_id = 1;
static volatile bool s_started;

/* touch state machine (guid task only) */
static bool    s_work_touch;              /* a finger went down in the client area */
static int     s_strip_zone = -1;         /* system button currently pressed, or -1 */

typedef struct { int kind, x, y; } inject_t;
static inject_t s_inject[INJECT_DEPTH];
static int      s_inj_head, s_inj_tail;

static uint32_t now_ms(void) { return (uint32_t)(hal_time_now_us() / 1000u); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------ window table */

static win_t *front(void) { return s_nord ? &s_win[s_order[s_nord - 1]] : NULL; }

static win_t *find(window_t id)
{
    for (int i = 0; i < WIN_MAX; i++) if (s_win[i].used && s_win[i].id == id) return &s_win[i];
    return NULL;
}

static uint32_t caller_app(void)
{
    task_t *t = task_current();
    return (t && t->app) ? t->app->app_id : 0;
}

/* A task may touch a window of its own app; kernel tasks may touch any. */
static bool can_access(const win_t *w)
{
    task_t *t = task_current();
    return !t || !t->app || t->app->app_id == w->owner_app;
}

static void order_remove(int idx)
{
    int k = 0;
    for (int i = 0; i < s_nord; i++) if (s_order[i] != idx) s_order[k++] = s_order[i];
    s_nord = k;
}

static void order_to_front(int idx)
{
    order_remove(idx);
    s_order[s_nord++] = idx;
}

/* --------------------------------------------------------------- events */

static void post(const win_t *w, gui_evt_type_t type, int x, int y)
{
    msg_t m = { .type = GUI_MSG_MAGIC | (uint32_t)type };
    m.data[0] = (uint8_t)w->id;  m.data[1] = (uint8_t)(w->id >> 8);
    m.data[2] = (uint8_t)(w->id >> 16); m.data[3] = (uint8_t)(w->id >> 24);
    m.data[4] = (uint8_t)x; m.data[5] = (uint8_t)((uint16_t)x >> 8);
    m.data[6] = (uint8_t)y; m.data[7] = (uint8_t)((uint16_t)y >> 8);
    msg_send_timeout(w->owner_pid, &m, 0);          /* never wait for a slow app; a full queue drops the event */
}

bool gui_decode(const msg_t *m, gui_event_t *ev)
{
    if ((m->type & 0xFFFF0000u) != GUI_MSG_MAGIC) return false;
    ev->type = (gui_evt_type_t)(m->type & 0xFFFFu);
    ev->window = m->data[0] | (uint32_t)m->data[1] << 8 | (uint32_t)m->data[2] << 16 | (uint32_t)m->data[3] << 24;
    ev->x = (int16_t)(m->data[4] | m->data[5] << 8);
    ev->y = (int16_t)(m->data[6] | m->data[7] << 8);
    return true;
}

int gui_next_event(gui_event_t *ev, uint32_t timeout_ms)
{
    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000u;
    for (;;) {
        uint64_t now = hal_time_now_us();
        uint32_t remaining = now >= deadline ? 0 : (uint32_t)((deadline - now) / 1000u);
        msg_t m;
        int rc = msg_receive_timeout(&m, remaining);
        if (rc != ERR_OK) return rc == ERR_NOMSG ? ERR_TIMEOUT : rc;
        if (gui_decode(&m, ev)) return ERR_OK;      /* anything else in the mailbox is not ours: skipped */
    }
}

/* -------------------------------------------------------------- painting */

static void draw_status(void)
{
    char buf[48];
    const win_t *w = front();
    uint32_t s = now_ms() / 1000u;
    display_fill_rect(0, 0, GUI_SCREEN_W, GUI_STATUS_H, COL_STATUS_BG);
    ksnprintf(buf, sizeof buf, "MeOS  %-15s up %02u:%02u", w ? w->title : "(no window)", (s / 60u) % 100u, s % 60u);
    display_draw_text(4, 4, buf, COL_STATUS_FG, COL_STATUS_BG, 1);
}

static void draw_strip(int pressed)
{
    static const char *const label[GUI_BUTTONS] = { "Home", "Next", "Close" };
    const int bw = GUI_SCREEN_W / GUI_BUTTONS;
    for (int i = 0; i < GUI_BUTTONS; i++) {
        uint16_t bg = i == pressed ? COL_BTN_DOWN : COL_BTN;
        int len = 0;
        while (label[i][len]) len++;
        display_fill_rect(i * bw + 2, GUI_STRIP_Y + 2, bw - 4, GUI_STRIP_H - 4, bg);
        display_draw_text(i * bw + (bw - len * 16) / 2, GUI_STRIP_Y + (GUI_STRIP_H - 16) / 2, label[i], COL_BTN_FG, bg, 2);
    }
}

static void draw_frame(const win_t *w)
{
    display_fill_rect(0, GUI_TITLE_Y, GUI_SCREEN_W, GUI_TITLE_H, COL_TITLE_BG);
    display_draw_text(4, GUI_TITLE_Y + 3, w->title, COL_TITLE_FG, COL_TITLE_BG, 1);
    display_fill_rect(0, GUI_CLIENT_Y, GUI_CLIENT_W, GUI_CLIENT_H, COL_DESKTOP);
}

static void draw_empty_desktop(void)
{
    display_fill_rect(0, GUI_TITLE_Y, GUI_SCREEN_W, GUI_CLIENT_H + GUI_TITLE_H, COL_DESKTOP);
    display_draw_text(60, 100, "no window - press Home", COL_STATUS_FG, COL_DESKTOP, 1);
}

/* Make the window on top of the stack visible: frame, status, and ask its owner to paint. */
static void show_front(void)
{
    win_t *w = front();
    if (w) { draw_frame(w); draw_status(); post(w, GUI_EVT_EXPOSE, 0, 0); }
    else   { draw_empty_desktop(); draw_status(); }
}

/* ----------------------------------------------------------- window API */

int window_create(const char *title, uint32_t flags)
{
    if (!task_has_perm(APP_PERM_DISPLAY)) return ERR_PERM;       /* permission first: do not reveal state to the unprivileged */
    if (!s_started) return ERR_NODEV;
    if (!title) return ERR_INVAL;

    mutex_lock(&s_lock);
    int slot = -1;
    for (int i = 0; i < WIN_MAX; i++) if (!s_win[i].used) { slot = i; break; }
    if (slot < 0) { mutex_unlock(&s_lock); return ERR_BUSY; }

    win_t *w = &s_win[slot];
    *w = (win_t){ .used = true, .id = s_next_id++, .owner_pid = task_current()->id,
                  .owner_app = caller_app(), .flags = flags };
    for (int i = 0; title[i] && i < TITLE_MAX - 1; i++) w->title[i] = title[i];
    order_to_front(slot);
    show_front();
    window_t id = w->id;
    mutex_unlock(&s_lock);
    return (int)id;
}

int window_focus(window_t id)
{
    mutex_lock(&s_lock);
    win_t *w = find(id);
    int rc = !w ? ERR_INVAL : !can_access(w) ? ERR_PERM : ERR_OK;
    if (rc == ERR_OK && front() != w) { order_to_front((int)(w - s_win)); show_front(); }
    mutex_unlock(&s_lock);
    return rc;
}

int window_draw(window_t id)
{
    mutex_lock(&s_lock);
    win_t *w = find(id);
    int rc = !w ? ERR_INVAL : !can_access(w) ? ERR_PERM : ERR_OK;
    if (rc == ERR_OK && front() == w) show_front();
    mutex_unlock(&s_lock);
    return rc;
}

static void remove_window(win_t *w)
{
    bool was_front = front() == w;
    order_remove((int)(w - s_win));
    w->used = false;
    if (was_front) show_front();
}

int window_destroy(window_t id)
{
    mutex_lock(&s_lock);
    win_t *w = find(id);
    int rc = !w ? ERR_INVAL : !can_access(w) ? ERR_PERM : ERR_OK;
    if (rc == ERR_OK) remove_window(w);
    mutex_unlock(&s_lock);
    return rc;
}

bool window_is_focused(window_t id)
{
    mutex_lock(&s_lock);
    bool f = front() && front()->id == id;
    mutex_unlock(&s_lock);
    return f;
}

/* Drawing: validate, then paint only if the window is in front.  (Not in front = it is not
 * on screen; the app will be asked to repaint when it comes back.) */

static bool clip(int *x, int *y, int *w, int *h)
{
    if (*w <= 0 || *h <= 0) return false;
    int x0 = imax(*x, 0), y0 = imax(*y, 0), x1 = imin(*x + *w, GUI_CLIENT_W), y1 = imin(*y + *h, GUI_CLIENT_H);
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return true;
}

static void paint_rect(int x, int y, int w, int h, uint16_t color)
{
    if (clip(&x, &y, &w, &h)) display_fill_rect(x, GUI_CLIENT_Y + y, w, h, color);
}

/* common prologue for the drawing calls: returns ERR_* or ERR_OK, sets *visible */
static int draw_check(window_t id, bool *visible)
{
    win_t *w = find(id);
    if (!w) return ERR_INVAL;
    if (!can_access(w)) return ERR_PERM;
    *visible = front() == w;
    return ERR_OK;
}

int window_fill_rect(window_t id, int x, int y, int w, int h, uint16_t color)
{
    mutex_lock(&s_lock);
    bool vis = false;
    int rc = draw_check(id, &vis);
    if (rc == ERR_OK && vis) paint_rect(x, y, w, h, color);
    mutex_unlock(&s_lock);
    return rc;
}

int window_clear(window_t id, uint16_t color) { return window_fill_rect(id, 0, 0, GUI_CLIENT_W, GUI_CLIENT_H, color); }

int window_draw_rect(window_t id, int x, int y, int w, int h, uint16_t color)
{
    mutex_lock(&s_lock);
    bool vis = false;
    int rc = draw_check(id, &vis);
    if (rc == ERR_OK && vis && w > 0 && h > 0) {
        paint_rect(x, y, w, 1, color);
        paint_rect(x, y + h - 1, w, 1, color);
        paint_rect(x, y, 1, h, color);
        paint_rect(x + w - 1, y, 1, h, color);
    }
    mutex_unlock(&s_lock);
    return rc;
}

int window_draw_text(window_t id, int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (!text) return ERR_INVAL;
    if (scale < 1) scale = 1;
    mutex_lock(&s_lock);
    bool vis = false;
    int rc = draw_check(id, &vis);
    /* text is all-or-nothing vertically and starts inside the client area; it is cut at the right edge */
    if (rc == ERR_OK && vis && x >= 0 && y >= 0 && x < GUI_CLIENT_W && y + 8 * scale <= GUI_CLIENT_H)
        display_draw_text(x, GUI_CLIENT_Y + y, text, fg, bg, scale);
    mutex_unlock(&s_lock);
    return rc;
}

int window_draw_bitmap(window_t id, int x, int y, int bw, int bh, const uint16_t *px)
{
    if (!px || bw <= 0 || bh <= 0 || x < 0 || y < 0 || x + bw > GUI_CLIENT_W || y + bh > GUI_CLIENT_H) return ERR_INVAL;
    mutex_lock(&s_lock);
    bool vis = false;
    int rc = draw_check(id, &vis);
    if (rc == ERR_OK && vis) display_draw_bitmap(x, GUI_CLIENT_Y + y, bw, bh, px);
    mutex_unlock(&s_lock);
    return rc;
}

/* ------------------------------------------------------------- inspection */

int gui_window_count(void)
{
    mutex_lock(&s_lock);
    int n = s_nord;
    mutex_unlock(&s_lock);
    return n;
}

int gui_front_title(char *out, size_t cap)
{
    mutex_lock(&s_lock);
    const win_t *w = front();
    int rc = w ? ERR_OK : ERR_NOENT;
    if (w && cap) { size_t i = 0; for (; w->title[i] && i + 1 < cap; i++) out[i] = w->title[i]; out[i] = 0; }
    mutex_unlock(&s_lock);
    return rc;
}

window_t gui_front_window(void)
{
    mutex_lock(&s_lock);
    window_t id = front() ? front()->id : 0;
    mutex_unlock(&s_lock);
    return id;
}

uint32_t gui_front_app(void)
{
    mutex_lock(&s_lock);
    uint32_t a = front() ? front()->owner_app : 0;
    mutex_unlock(&s_lock);
    return a;
}

void gui_print_windows(void)
{
    mutex_lock(&s_lock);
    kprintf("ID   TITLE            OWNER(pid)  APP   FLAGS\n");
    for (int i = s_nord - 1; i >= 0; i--) {                      /* front first */
        const win_t *w = &s_win[s_order[i]];
        kprintf("%-4u %-16s %-10u  %-4u  %s%s\n", w->id, w->title, w->owner_pid, w->owner_app,
                w->flags & GUI_WIN_HOME ? "home " : "", i == s_nord - 1 ? "(front)" : "");
    }
    if (!s_nord) kprintf("(no windows)\n");
    mutex_unlock(&s_lock);
}

/* ------------------------------------------------- app-level operations */

int gui_focus_app(uint32_t app_id)
{
    mutex_lock(&s_lock);
    int rc = ERR_NOENT;
    for (int i = s_nord - 1; i >= 0; i--) {
        win_t *w = &s_win[s_order[i]];
        if (w->owner_app == app_id) {
            if (front() != w) { order_to_front((int)(w - s_win)); show_front(); }
            rc = ERR_OK;
            break;
        }
    }
    mutex_unlock(&s_lock);
    return rc;
}

/* An app ended (called by appd): its windows must not outlive it. */
void gui_release_owner(uint32_t app_id)
{
    if (!s_started || !app_id) return;
    mutex_lock(&s_lock);
    for (int i = 0; i < WIN_MAX; i++) if (s_win[i].used && s_win[i].owner_app == app_id) remove_window(&s_win[i]);
    mutex_unlock(&s_lock);
}

/* ---------------------------------------------------------- touch -> events */

/* The three system buttons. */
static void system_button(int zone)
{
    win_t *f = front();
    if (zone == 0) {                                              /* Home */
        for (int i = s_nord - 1; i >= 0; i--) {
            win_t *w = &s_win[s_order[i]];
            if (w->flags & GUI_WIN_HOME) { if (front() != w) { order_to_front((int)(w - s_win)); show_front(); } return; }
        }
    } else if (zone == 1) {                                       /* Next: send the front window to the back */
        if (s_nord > 1) { order_to_front(s_order[0]); show_front(); }
    } else if (f && !(f->flags & GUI_WIN_HOME)) {                 /* Close: ask politely; guid enforces it later */
        f->close_deadline_ms = now_ms() + CLOSE_GRACE_MS;
        post(f, GUI_EVT_CLOSE, 0, 0);
    }
}

static void handle_touch(int kind, int x, int y)
{
    mutex_lock(&s_lock);
    if (kind == GUI_TOUCH_DOWN) {
        if (y >= GUI_STRIP_Y) {
            int zone = imin(x * GUI_BUTTONS / GUI_SCREEN_W, GUI_BUTTONS - 1);
            s_strip_zone = imax(zone, 0);
            draw_strip(s_strip_zone);
            system_button(s_strip_zone);
        } else if (y >= GUI_CLIENT_Y && front()) {
            s_work_touch = true;
            post(front(), GUI_EVT_TOUCH_DOWN, x, y - GUI_CLIENT_Y);
        }
    } else if (kind == GUI_TOUCH_MOVE) {
        if (s_work_touch && front()) post(front(), GUI_EVT_TOUCH_MOVE, x, y - GUI_CLIENT_Y);
    } else {
        if (s_strip_zone >= 0) { s_strip_zone = -1; draw_strip(-1); }
        if (s_work_touch && front()) post(front(), GUI_EVT_TOUCH_UP, x, y - GUI_CLIENT_Y);
        s_work_touch = false;
    }
    mutex_unlock(&s_lock);
}

void gui_inject_touch(int kind, int x, int y)
{
    uint32_t irq = arch_irq_disable();
    int next = (s_inj_head + 1) % INJECT_DEPTH;
    if (next != s_inj_tail) { s_inject[s_inj_head] = (inject_t){ kind, x, y }; s_inj_head = next; }
    arch_irq_restore(irq);
}

static bool pop_injected(inject_t *out)
{
    bool got = false;
    uint32_t irq = arch_irq_disable();
    if (s_inj_tail != s_inj_head) { *out = s_inject[s_inj_tail]; s_inj_tail = (s_inj_tail + 1) % INJECT_DEPTH; got = true; }
    arch_irq_restore(irq);
    return got;
}

/* Close was pressed and the app did not leave in time: stop it. */
static void enforce_close_deadlines(void)
{
    uint32_t stop_app = 0;
    mutex_lock(&s_lock);
    uint32_t now = now_ms();
    for (int i = 0; i < WIN_MAX; i++) {
        win_t *w = &s_win[i];
        if (w->used && w->close_deadline_ms && (int32_t)(now - w->close_deadline_ms) >= 0) {
            w->close_deadline_ms = 0;
            if (w->owner_app) stop_app = w->owner_app;
        }
    }
    mutex_unlock(&s_lock);
    if (stop_app) app_stop(stop_app);                             /* appd then removes its windows */
}

static void guid_task(void *arg)
{
    (void)arg;
    int btn = vfs_open("/dev/buttons", VFS_O_READ);
    bool down = false;
    int lx = 0, ly = 0;
    uint32_t last_status = now_ms();

    for (;;) {
        task_sleep(POLL_MS);

        inject_t e;
        while (pop_injected(&e)) handle_touch(e.kind, e.x, e.y);

        if (btn >= 0) {
            buttons_touch_t t;
            if (vfs_ioctl(btn, BUTTONS_IOC_GET_TOUCH, &t) == ERR_OK) {
                if (t.pressed && !down)      { down = true; lx = t.x; ly = t.y; handle_touch(GUI_TOUCH_DOWN, lx, ly); }
                else if (t.pressed && down) {
                    if (imax(t.x - lx, lx - t.x) >= MOVE_THRESHOLD || imax(t.y - ly, ly - t.y) >= MOVE_THRESHOLD) {
                        lx = t.x; ly = t.y;
                        handle_touch(GUI_TOUCH_MOVE, lx, ly);
                    }
                } else if (!t.pressed && down) { down = false; handle_touch(GUI_TOUCH_UP, lx, ly); }
            }
        }

        enforce_close_deadlines();

        if (now_ms() - last_status >= STATUS_PERIOD_MS) {
            last_status = now_ms();
            mutex_lock(&s_lock);
            draw_status();
            mutex_unlock(&s_lock);
        }
    }
}

int gui_init(void)
{
    if (s_started) return ERR_OK;
    mutex_lock(&s_lock);
    display_clear(COL_DESKTOP);
    draw_empty_desktop();
    draw_status();
    draw_strip(-1);
    s_started = true;
    mutex_unlock(&s_lock);
    int pid = task_create_for_app("guid", guid_task, NULL, GUID_PRIO, GUID_STACK, NULL);
    return pid > 0 ? ERR_OK : pid;
}
