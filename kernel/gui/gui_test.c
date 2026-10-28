/*
 * GUI self test.  Nothing here needs a person: the screen contents cannot be read back, so the
 * logic is tested as pure functions, and the window system through its API, its events and
 * simulated touches (gui_inject_touch, the same path the real panel takes).
 */
#include <m5os/gui.h>
#include <m5os/app.h>
#include <m5os/task.h>
#include <m5os/errno.h>
#include <m5os/msg.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include "../../apps/apps.h"
#include "../../apps/calculator/calc.h"
#include "../../apps/snake/snake_logic.h"
#include "../../apps/clock/clock_math.h"

#define CK(c) do { if (!(c)) { kprintf("  FAIL line %d: %s\n", __LINE__, #c); ok = false; } } while (0)
#define SETTLE_MS 300u
/* Poll a condition instead of sleeping a fixed time: painting through the bit-banged SPI bus takes
 * tens of milliseconds per button, so how long a tap takes to show up depends on the load. */
#define WAIT(cond, ms) ({ bool _w = false; for (int _t = 0; _t < (ms) / 20; _t++) { if (cond) { _w = true; break; } task_sleep(20); } _w; })

static bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* ---- pure logic ----------------------------------------------------------- */

static bool calc_shows(const char *keys, const char *expect)
{
    calc_t c;
    calc_init(&c);
    for (const char *k = keys; *k; k++) calc_press(&c, *k);
    char buf[16];
    calc_display(&c, buf, sizeof buf);
    if (!streq(buf, expect)) kprintf("  calc \"%s\" -> \"%s\" (expected \"%s\")\n", keys, buf, expect);
    return streq(buf, expect);
}

static bool test_calculator(void)
{
    bool ok = true;
    CK(calc_shows("12+30=", "42"));
    CK(calc_shows("7*8=", "56"));
    CK(calc_shows("100/4=", "25"));
    CK(calc_shows("2+3*4=", "20"));                    /* immediate execution: (2+3)*4 */
    CK(calc_shows("9-12=", "-3"));
    CK(calc_shows("5+=", "10"));
    CK(calc_shows("3*=", "9"));
    CK(calc_shows("2+3=+4=", "9"));                    /* continue from a result */
    CK(calc_shows("2+3=7", "7"));                      /* a digit after '=' starts over */
    CK(calc_shows("7", "7"));
    CK(calc_shows("0007", "7"));                       /* leading zeros */
    CK(calc_shows("1234567890", "123456789"));         /* nine digits at most */
    CK(calc_shows("-5=", "-5"));                       /* an operator first acts on 0 */
    CK(calc_shows("6*/2=", "3"));                      /* two operators in a row: the last wins */
    CK(calc_shows("5/0=", "Error"));
    CK(calc_shows("5/0+3", "Error"));                  /* an error blocks everything ... */
    CK(calc_shows("5/0C7+1=", "8"));                   /* ... until C */
    CK(calc_shows("999999999+1=", "Error"));           /* overflow */
    CK(calc_shows("12+30=C", "0"));
    CK(calc_shows("7/2=", "3"));                       /* integers: truncating division */
    CK(calc_shows("0-7/2=", "-3"));                    /* (0-7)/2 by immediate execution; -7/2 truncates toward zero */
    return ok;
}

static bool test_snake(void)
{
    bool ok = true;
    static snake_t s;
    snake_init(&s, 1234);
    CK(s.len == SNAKE_START_LEN && s.x[0] == SNAKE_COLS / 2 && s.y[0] == SNAKE_ROWS / 2 && s.x[2] == SNAKE_COLS / 2 - 2);
    CK(!(s.food_x == s.x[0] && s.food_y == s.y[0]));
    s.food_x = 0; s.food_y = 0;                                      /* keep food out of the way */

    CK(snake_step(&s) == SNAKE_MOVED && s.x[0] == SNAKE_COLS / 2 + 1 && s.len == SNAKE_START_LEN);
    snake_turn(&s, SNAKE_LEFT);                                      /* reversing into the neck is ignored */
    CK(snake_step(&s) == SNAKE_MOVED && s.x[0] == SNAKE_COLS / 2 + 2);
    snake_turn(&s, SNAKE_UP);
    CK(snake_step(&s) == SNAKE_MOVED && s.y[0] == SNAKE_ROWS / 2 - 1);

    snake_init(&s, 99); s.food_x = 0; s.food_y = 0;                  /* walls */
    int steps = 0;
    while (snake_step(&s) != SNAKE_DEAD && steps < 100) steps++;
    CK(s.dead && steps == SNAKE_COLS - 1 - SNAKE_COLS / 2);         /* dies on the step that would leave the board */
    CK(snake_step(&s) == SNAKE_DEAD);

    snake_init(&s, 7);                                               /* eating */
    s.food_x = (uint8_t)(s.x[0] + 1); s.food_y = s.y[0];
    CK(snake_step(&s) == SNAKE_ATE && s.len == SNAKE_START_LEN + 1 && s.score == 1);
    CK(!(s.food_x == s.x[0] && s.food_y == s.y[0]));
    bool food_on_body = false;
    for (int i = 0; i < s.len; i++) if (s.x[i] == s.food_x && s.y[i] == s.food_y) food_on_body = true;
    CK(!food_on_body);                                               /* new food never lands on the snake */

    snake_init(&s, 7);                                               /* running into itself */
    s.len = 5; s.dir = s.next_dir = SNAKE_DOWN;
    s.x[0] = 5; s.y[0] = 5; s.x[1] = 6; s.y[1] = 5; s.x[2] = 6; s.y[2] = 6; s.x[3] = 5; s.y[3] = 6; s.x[4] = 4; s.y[4] = 6;
    s.food_x = 30; s.food_y = 15;
    CK(snake_step(&s) == SNAKE_DEAD);

    snake_init(&s, 7);                                               /* the tail cell is free to enter */
    s.len = 4; s.dir = s.next_dir = SNAKE_DOWN;
    s.x[0] = 5; s.y[0] = 5; s.x[1] = 6; s.y[1] = 5; s.x[2] = 6; s.y[2] = 6; s.x[3] = 5; s.y[3] = 6;
    s.food_x = 30; s.food_y = 15;
    CK(snake_step(&s) == SNAKE_MOVED && s.x[0] == 5 && s.y[0] == 6);
    return ok;
}

static bool test_clock_math(void)
{
    bool ok = true;
    CK(clock_sin(0) == 0 && clock_sin(15) == 256 && clock_sin(30) == 0 && clock_sin(45) == -256);
    CK(clock_cos(0) == 256 && clock_cos(15) == 0 && clock_cos(30) == -256);
    int x, y;
    clock_point(100, 100, 0, 50, &x, &y);   CK(x == 100 && y == 50);      /* 12 o'clock: up */
    clock_point(100, 100, 15, 50, &x, &y);  CK(x == 150 && y == 100);     /* 3 */
    clock_point(100, 100, 30, 50, &x, &y);  CK(x == 100 && y == 150);     /* 6 */
    clock_point(100, 100, 45, 50, &x, &y);  CK(x == 50 && y == 100);      /* 9 */
    clock_point(100, 100, -15, 50, &x, &y); CK(x == 50 && y == 100);      /* negative positions wrap */
    return ok;
}

/* ---- window system ---------------------------------------------------------- */

static bool app_running(const char *name, uint32_t *id)
{
    app_info_t all[APP_MAX];
    int n = app_list(all, APP_MAX);
    for (int i = 0; i < n; i++) if (all[i].state == APP_RUNNING && streq(all[i].name, name)) { if (id) *id = all[i].app_id; return true; }
    return false;
}

static bool front_is(const char *title)
{
    char t[20];
    return gui_front_title(t, sizeof t) == ERR_OK && streq(t, title);
}

static void dump_state(const char *where)
{
    app_info_t all[APP_MAX];
    int n = app_list(all, APP_MAX);
    kprintf("  --- state at %s ---\n", where);
    for (int i = 0; i < n; i++)
        kprintf("  app %u %s %s pid %u tasks %u\n", all[i].app_id, all[i].name, app_state_name(all[i].state), all[i].owner_task, all[i].tasks);
    gui_print_windows();
    task_info_t t[TASK_MAX];
    int nt = task_snapshot(t, TASK_MAX);
    for (int i = 0; i < nt; i++) kprintf("  task %u %s %s app %u\n", t[i].id, t[i].name, task_state_name(t[i].state), t[i].app_id);
}

static void drain(void) { msg_t m; while (msg_try_receive(&m) == ERR_OK) { } }

static void tap(int x, int y)
{
    gui_inject_touch(GUI_TOUCH_DOWN, x, y);
    task_sleep(70);
    gui_inject_touch(GUI_TOUCH_UP, x, y);
    task_sleep(70);
}

#define LAUNCHER_BTN_X     160
#define LAUNCHER_BTN_Y(i)  (GUI_CLIENT_Y + 2 + (i) * 42 + 18)
#define KEY_X(c)           ((c) * 80 + 40)
#define KEY_Y(r)           (GUI_CLIENT_Y + 44 + (r) * 31 + 15)
#define STRIP_Y            (GUI_STRIP_Y + 20)
#define BTN_HOME_X         20
#define BTN_NEXT_X         160
#define BTN_CLOSE_X        280

bool gui_selftest(void)
{
    bool ok = true;
    kmem_stats_t base, now;

    ok &= test_calculator();
    ok &= test_snake();
    ok &= test_clock_math();

    /* the boot-time demo tasks are in the way: the log writers (leak baseline), the CPU hogs (they make
     * every repaint take several times longer) and the demo status screen (it paints the whole display) */
    for (int i = 0; i < 600 && (msg_lookup("logA") > 0 || msg_lookup("logB") > 0); i++) task_sleep(50);
    { static const char *const demo[] = { "hog0", "hog1", "ui" };
      for (unsigned i = 0; i < sizeof demo / sizeof demo[0]; i++) { int pid = msg_lookup(demo[i]); if (pid > 0) task_kill((uint32_t)pid); } }
    task_sleep(SETTLE_MS);
    CK(gui_init() == ERR_OK);
    task_sleep(SETTLE_MS);
    drain();
    kmem_internal_stats(&base);

    /* ---- windows: create, front, focus, destroy ------------------------------- */
    gui_event_t ev;
    int a = window_create("Test A", 0);
    CK(a > 0);
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_EXPOSE && ev.window == (window_t)a);
    int b = window_create("Test B", 0);
    CK(b > 0 && b != a);
    CK(front_is("Test B") && window_is_focused((window_t)b) && !window_is_focused((window_t)a));
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_EXPOSE && ev.window == (window_t)b);
    CK(gui_window_count() == 2);

    CK(window_fill_rect((window_t)a, 0, 0, 10, 10, 0) == ERR_OK);           /* behind: accepted, simply not shown */
    CK(window_fill_rect(9999, 0, 0, 10, 10, 0) == ERR_INVAL);
    CK(window_clear(9999, 0) == ERR_INVAL && window_draw_rect(9999, 0, 0, 5, 5, 0) == ERR_INVAL);
    CK(window_draw_text((window_t)b, 0, 0, NULL, 0, 0, 1) == ERR_INVAL);
    CK(window_draw_text(9999, 0, 0, "x", 0, 0, 1) == ERR_INVAL);
    CK(window_draw_text((window_t)b, 300, 5, "clipped at the edge", 0xFFFF, 0, 1) == ERR_OK);
    CK(window_draw_text((window_t)b, -4, 5, "off the left", 0xFFFF, 0, 1) == ERR_OK);
    CK(window_fill_rect((window_t)b, -50, -50, 1000, 1000, 0) == ERR_OK);   /* clipped to the client area */
    static const uint16_t px[4] = { 1, 2, 3, 4 };
    CK(window_draw_bitmap((window_t)b, 10, 10, 2, 2, px) == ERR_OK);
    CK(window_draw_bitmap((window_t)b, 319, 10, 2, 2, px) == ERR_INVAL);    /* does not fit */
    CK(window_draw_bitmap((window_t)b, 10, 10, 2, 2, NULL) == ERR_INVAL);

    CK(window_focus((window_t)a) == ERR_OK && front_is("Test A"));
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_EXPOSE && ev.window == (window_t)a);
    CK(window_focus(9999) == ERR_INVAL);
    CK(window_draw((window_t)a) == ERR_OK);
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_EXPOSE);    /* window_draw asks the owner to repaint */
    CK(window_destroy((window_t)a) == ERR_OK);
    CK(front_is("Test B") && gui_window_count() == 1);                       /* focus falls back to what is below */
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_EXPOSE && ev.window == (window_t)b);
    CK(window_destroy((window_t)a) == ERR_INVAL);

    /* ---- touch: client area -> events with client coordinates ----------------- */
    gui_inject_touch(GUI_TOUCH_DOWN, 100, GUI_CLIENT_Y + 70);
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_TOUCH_DOWN && ev.x == 100 && ev.y == 70 && ev.window == (window_t)b);
    gui_inject_touch(GUI_TOUCH_MOVE, 110, GUI_CLIENT_Y + 90);
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_TOUCH_MOVE && ev.x == 110 && ev.y == 90);
    gui_inject_touch(GUI_TOUCH_UP, 110, GUI_CLIENT_Y + 90);
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_TOUCH_UP && ev.x == 110 && ev.y == 90);
    tap(100, GUI_TITLE_Y + 5);                                                /* title bar and status bar: no events */
    tap(100, 5);
    CK(gui_next_event(&ev, 200) == ERR_TIMEOUT);

    /* ---- the system buttons ------------------------------------------------------ */
    tap(BTN_NEXT_X, STRIP_Y);                                                 /* one window: nothing to switch to */
    tap(BTN_HOME_X, STRIP_Y);                                                 /* no home window yet */
    CK(front_is("Test B"));
    drain();
    tap(BTN_CLOSE_X, STRIP_Y);                                                /* Close = ask the owner politely */
    CK(gui_next_event(&ev, 300) == ERR_OK && ev.type == GUI_EVT_CLOSE && ev.window == (window_t)b);
    CK(window_destroy((window_t)b) == ERR_OK);
    CK(gui_window_count() == 0 && gui_front_app() == 0 && window_destroy((window_t)b) == ERR_INVAL);
    drain();

    /* ---- permissions ----------------------------------------------------------------- */
    int own = window_create("Mine", 0);
    CK(own > 0);
    app_intruder_target = (uint32_t)own; app_intruder_result = 0;
    uint32_t id;
    CK(app_start("intruder", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(app_intruder_result == 7u);                                            /* draw, destroy and focus of a foreign window: all refused */
    CK(front_is("Mine"));
    CK(window_destroy((window_t)own) == ERR_OK);
    drain();

    /* ---- a window belongs to its app: it goes away with it ------------------------- */
    CK(app_start("clock", &id) == ERR_OK);
    task_sleep(SETTLE_MS);
    CK(gui_window_count() == 1 && front_is("Clock") && gui_front_app() == id);
    CK(app_stop(id) == ERR_OK);                                               /* forced stop: no Close was sent */
    task_sleep(SETTLE_MS);
    CK(gui_window_count() == 0);

    /* ---- launcher, end to end, through simulated touches ------------------------- */
    CK(app_start("launcher", &id) == ERR_OK);
    uint32_t launcher_id = id;
    CK(WAIT(front_is("MeOS"), 3000));
    task_sleep(600);                                                          /* its first paint (4 buttons) */

    tap(LAUNCHER_BTN_X, LAUNCHER_BTN_Y(2));                                   /* [ Calculator ] */
    uint32_t calc_id = 0;
    CK(WAIT(app_running("calculator", &calc_id) && front_is("Calculator"), 4000));
    if (!app_running("calculator", NULL)) dump_state("after tapping Calculator");

    tap(BTN_HOME_X, STRIP_Y);                                                 /* Home */
    CK(WAIT(front_is("MeOS"), 3000));
    tap(LAUNCHER_BTN_X, LAUNCHER_BTN_Y(2));                                   /* already running: focus it, do not start another */
    CK(WAIT(front_is("Calculator"), 4000));
    int running = 0;
    { app_info_t all[APP_MAX]; int n = app_list(all, APP_MAX); for (int i = 0; i < n; i++) if (all[i].state == APP_RUNNING && streq(all[i].name, "calculator")) running++; }
    CK(running == 1);

    tap(BTN_NEXT_X, STRIP_Y);                                                 /* Next: send the front window back */
    CK(WAIT(front_is("MeOS"), 3000));
    tap(LAUNCHER_BTN_X, LAUNCHER_BTN_Y(2));                                   /* back to the calculator */
    CK(WAIT(front_is("Calculator"), 4000));

    uint32_t painted = app_calc_painted;
    window_t calc_win = gui_front_window();
    (void)calc_win;
    CK(WAIT(app_calc_painted >= painted && streq(app_calc_display, "0"), 3000));
    task_sleep(600);                                                          /* the keypad is repainted after each EXPOSE */
    tap(KEY_X(0), KEY_Y(0)); tap(KEY_X(3), KEY_Y(3));                         /* 7 + */
    tap(KEY_X(1), KEY_Y(0)); tap(KEY_X(2), KEY_Y(3));                         /* 8 = */
    CK(WAIT(streq(app_calc_display, "15"), 3000));                            /* the keypad really calculates */
    tap(KEY_X(0), KEY_Y(3));                                                  /* C */
    CK(WAIT(streq(app_calc_display, "0"), 3000));

    tap(BTN_CLOSE_X, STRIP_Y);                                                /* Close: the app is told, leaves, its window goes */
    CK(WAIT(!app_running("calculator", NULL) && front_is("MeOS"), 5000));
    { app_info_t all[APP_MAX]; int n = app_list(all, APP_MAX); app_info_t info = { 0 }; for (int i = 0; i < n; i++) if (all[i].app_id == calc_id) info = all[i]; CK(info.state == APP_EXITED); }
    CK(gui_window_count() == 1);

    tap(BTN_CLOSE_X, STRIP_Y);                                                /* Close on the home window does nothing */
    task_sleep(SETTLE_MS + 300);
    CK(app_running("launcher", NULL) && front_is("MeOS"));

    /* the terminal: its buttons type into the console input */
    while (kconsole_getc() >= 0) { }
    tap(LAUNCHER_BTN_X, LAUNCHER_BTN_Y(0));                                   /* [ Terminal ] */
    CK(WAIT(front_is("Terminal"), 4000));
    task_sleep(800);                                                          /* its first paint: 18 text rows */
    tap(26, GUI_CLIENT_Y + 18 * 8 + 2 + 11);                                  /* the "ps" button */
    CK(WAIT(kconsole_getc() == 'p', 2000));
    CK(WAIT(kconsole_getc() == 's', 500) && WAIT(kconsole_getc() == '\r', 500));
    tap(BTN_CLOSE_X, STRIP_Y);
    CK(WAIT(!app_running("terminal", NULL) && front_is("MeOS"), 5000));

    tap(LAUNCHER_BTN_X, LAUNCHER_BTN_Y(3));                                   /* [ Snake ] */
    CK(WAIT(front_is("Snake") && app_running("snake", NULL), 4000));
    task_sleep(500);
    gui_inject_touch(GUI_TOUCH_DOWN, 300, GUI_CLIENT_Y + 100);                /* steering input must not disturb it */
    task_sleep(70);
    gui_inject_touch(GUI_TOUCH_UP, 300, GUI_CLIENT_Y + 100);
    task_sleep(400);
    CK(app_running("snake", NULL));
    tap(BTN_CLOSE_X, STRIP_Y);
    CK(WAIT(!app_running("snake", NULL) && front_is("MeOS"), 5000));

    tap(LAUNCHER_BTN_X, LAUNCHER_BTN_Y(1));                                   /* [ Clock ] */
    CK(WAIT(front_is("Clock"), 4000));
    task_sleep(500);
    tap(BTN_CLOSE_X, STRIP_Y);
    CK(WAIT(!app_running("clock", NULL) && front_is("MeOS"), 5000));

    /* an app that has no window is still stoppable (Close cannot reach it) */
    CK(app_start("hog", &id) == ERR_OK);
    task_sleep(50);
    CK(app_stop(id) == ERR_OK);
    task_sleep(SETTLE_MS);

    /* ---- clean up and check nothing is left ----------------------------------------- */
    CK(app_stop(launcher_id) == ERR_OK);
    CK(WAIT(gui_window_count() == 0, 3000));
    task_sleep(SETTLE_MS);
    drain();
    kmem_internal_stats(&now);
    CK(now.used == base.used);
    if (now.used != base.used) kprintf("  heap used changed by %d bytes\n", (int)(now.used - base.used));
    kprintf("gui selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
