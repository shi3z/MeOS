/*
 * M5OS Graphical Wi-Fi Configuration App (Tab5 + Keyboard)
 *
 * Features:
 *  - Scans available 2.4 GHz Wi-Fi networks via the onboard ESP32-C6 co-processor
 *  - Renders graphical signal-strength bars, security badges, channel, RSSI, and IP status
 *  - Keyboard selection (Up/Down/j/k, Enter to enter password & connect)
 *  - Persists credentials to /home/wifi.cfg and ESP32-C6 NVS flash for automatic boot connection
 *  - Supports disconnecting, toggling auto-connect, and forgetting networks from GUI or CLI
 */
#include "wifi_app.h"
#include "../tui/dqwin.h"
#include "../../drivers/lcd/fbcon.h"
#include <m5os/wifi.h>
#include <m5os/display.h>
#include <m5os/kprintf.h>
#include <m5os/hal_time.h>
#include <m5os/task.h>

#define CELL_PX         (8 * M5OS_FBCON_SCALE)
#define HEADER_ROWS     7
#define FOOTER_ROWS     4

static wifi_ap_t s_aps[WIFI_MAX_SCAN_APS];
static int       s_ap_count;
static int       s_sel;
static int       s_top;
static bool      s_full_redraw;

static int str_len(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static bool str_eq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void draw_padded_color(int cx, int cy, int width_cells, const char *s, uint16_t fg, uint16_t bg)
{
    char line[160];
    int n = 0;
    if (width_cells > (int)sizeof(line) - 1) width_cells = (int)sizeof(line) - 1;
    while (s && s[n] && n < width_cells) {
        line[n] = s[n];
        n++;
    }
    while (n < width_cells) line[n++] = ' ';
    line[n] = 0;
    display_draw_text(cx * CELL_PX, cy * CELL_PX, line, fg, bg, M5OS_FBCON_SCALE);
}

/* Draw a graphical 4-bar Wi-Fi signal strength meter inside a 2-cell wide box at (cx, cy) */
static void draw_signal_bars(int cx, int cy, int8_t rssi, bool active, uint16_t bg)
{
    int x0 = cx * CELL_PX;
    int y0 = cy * CELL_PX;
    int w  = 2 * CELL_PX;
    int h  = CELL_PX;
    display_fill_rect(x0, y0, w, h, bg);

    int bars = 0;
    if (active) {
        if (rssi >= -55)      bars = 4;
        else if (rssi >= -67) bars = 3;
        else if (rssi >= -78) bars = 2;
        else                  bars = 1;
    }

    uint16_t bar_color = COLOR_GREEN;
    if (bars == 2) bar_color = COLOR_YELLOW;
    else if (bars <= 1) bar_color = RGB565(255, 120, 0);

    int bw  = (3 * M5OS_FBCON_SCALE);
    int gap = (1 * M5OS_FBCON_SCALE);
    int bx0 = x0 + 2 * M5OS_FBCON_SCALE;

    for (int b = 0; b < 4; b++) {
        int bh = (2 + b * 2) * M5OS_FBCON_SCALE;
        if (bh > h - 2) bh = h - 2;
        int bx = bx0 + b * (bw + gap);
        int by = y0 + h - 1 - bh;
        uint16_t c = (b < bars) ? bar_color : RGB565(60, 60, 60);
        display_fill_rect(bx, by, bw, bh, c);
    }
}

/* Draw a colored pill/badge at (cx, cy) */
static void draw_badge(int cx, int cy, const char *label, uint16_t fg, uint16_t bg)
{
    int len = str_len(label);
    int x = cx * CELL_PX;
    int y = cy * CELL_PX;
    display_fill_rect(x, y, len * CELL_PX, CELL_PX, bg);
    display_draw_text(x, y, label, fg, bg, M5OS_FBCON_SCALE);
}

static int list_rows(void)
{
    int r = dq_rows() - HEADER_ROWS - FOOTER_ROWS - 4;
    return r < 4 ? 4 : r;
}

static void show_busy_box(const char *line1, const char *line2)
{
    int w = 52;
    if (w > dq_cols() - 4) w = dq_cols() - 4;
    int h = 6;
    int x = (dq_cols() - w) / 2;
    int y = (dq_rows() - h) / 2;
    dq_window(x, y, w, h);
    draw_padded_color(x + 2, y + 2, w - 4, line1, COLOR_YELLOW, COLOR_BLACK);
    if (line2) {
        draw_padded_color(x + 2, y + 3, w - 4, line2, COLOR_CYAN, COLOR_BLACK);
    }
}

static void refresh_ap_flags(const wifi_status_t *st)
{
    for (int i = 0; i < s_ap_count; i++) {
        s_aps[i].saved     = st->has_saved && str_eq(s_aps[i].ssid, st->saved_ssid);
        s_aps[i].connected = (st->state == WIFI_STATE_CONNECTED) && str_eq(s_aps[i].ssid, st->ssid);
    }
}

static void do_scan(void)
{
    show_busy_box("Scanning 2.4 GHz Wi-Fi networks...", "ESP32-C6 SDIO active scan in progress");
    int n = wifi_scan(s_aps, WIFI_MAX_SCAN_APS);
    if (n >= 0) {
        s_ap_count = n;
        if (s_sel >= s_ap_count) s_sel = s_ap_count > 0 ? s_ap_count - 1 : 0;
        if (s_sel < 0) s_sel = 0;
        s_top = 0;
    }
    s_full_redraw = true;
}

static void draw_header(const wifi_status_t *st)
{
    int w = dq_cols();
    if (s_full_redraw) {
        dq_window(0, 0, w, HEADER_ROWS);
    }

    draw_padded_color(2, 1, 26, "MeOS WI-FI SETTINGS", COLOR_CYAN, COLOR_BLACK);

    /* Status badge + signal indicator */
    if (st->state == WIFI_STATE_CONNECTED) {
        draw_badge(28, 1, " CONNECTED ", COLOR_BLACK, COLOR_GREEN);
        draw_signal_bars(40, 1, st->rssi, true, COLOR_BLACK);
    } else if (st->state == WIFI_STATE_CONNECTING || st->state == WIFI_STATE_SCANNING) {
        draw_badge(28, 1, " WORKING.. ", COLOR_BLACK, COLOR_YELLOW);
        draw_signal_bars(40, 1, -90, false, COLOR_BLACK);
    } else if (st->state == WIFI_STATE_ERROR) {
        draw_badge(28, 1, " HW ERROR  ", COLOR_WHITE, COLOR_RED);
        draw_signal_bars(40, 1, -90, false, COLOR_BLACK);
    } else {
        draw_badge(28, 1, " DISCONN.  ", COLOR_WHITE, RGB565(80, 80, 80));
        draw_signal_bars(40, 1, -90, false, COLOR_BLACK);
    }

    char auto_str[32];
    ksnprintf(auto_str, sizeof auto_str, "Auto-Connect: %s", st->auto_connect ? "[ON] " : "[OFF]");
    draw_padded_color(w - 22, 1, 20, auto_str, st->auto_connect ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);

    char line2[120];
    if (st->state == WIFI_STATE_CONNECTED) {
        ksnprintf(line2, sizeof line2,
                  "AP: %-24s  Ch: %2u  RSSI: %d dBm  Sec: %s",
                  st->ssid, st->channel, (int)st->rssi, wifi_auth_name(st->authmode));
    } else {
        ksnprintf(line2, sizeof line2,
                  "AP: (not connected)           Saved: %s",
                  st->has_saved ? st->saved_ssid : "(none)");
    }
    draw_padded_color(2, 2, w - 4, line2, COLOR_WHITE, COLOR_BLACK);

    char line3[120];
    ksnprintf(line3, sizeof line3,
              "IP: %u.%u.%u.%u   GW: %u.%u.%u.%u   MAC: %02X:%02X:%02X:%02X:%02X:%02X",
              st->ip[0], st->ip[1], st->ip[2], st->ip[3],
              st->gateway[0], st->gateway[1], st->gateway[2], st->gateway[3],
              st->mac[0], st->mac[1], st->mac[2], st->mac[3], st->mac[4], st->mac[5]);
    draw_padded_color(2, 3, w - 4, line3,
                      st->ip[0] ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);

    char line4[120];
    ksnprintf(line4, sizeof line4, "Status: %s", st->last_msg);
    draw_padded_color(2, 5, w - 4, line4, COLOR_YELLOW, COLOR_BLACK);
}

static void draw_ap_list(const wifi_status_t *st)
{
    int w = dq_cols();
    int lrows = list_rows();
    int win_y = HEADER_ROWS;
    int win_h = lrows + 4;

    if (s_full_redraw) {
        dq_window(0, win_y, w, win_h);
        draw_padded_color(2, win_y + 1, w - 4,
                          "   SIG  SSID                              SECURITY  CH   RSSI      STATUS",
                          COLOR_CYAN, COLOR_BLACK);
    }

    refresh_ap_flags(st);

    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + lrows) s_top = s_sel - lrows + 1;
    if (s_top < 0) s_top = 0;

    for (int r = 0; r < lrows; r++) {
        int idx = s_top + r;
        int cy  = win_y + 2 + r;

        if (idx >= s_ap_count) {
            if (r == 0 && s_ap_count == 0) {
                draw_padded_color(2, cy, w - 4,
                                  "  (No networks listed - press R to scan nearby Wi-Fi APs)",
                                  COLOR_GRAY, COLOR_BLACK);
            } else {
                draw_padded_color(2, cy, w - 4, "", COLOR_WHITE, COLOR_BLACK);
            }
            continue;
        }

        const wifi_ap_t *ap = &s_aps[idx];
        bool sel = (idx == s_sel);
        uint16_t row_bg = sel ? RGB565(25, 65, 130) : COLOR_BLACK;
        uint16_t row_fg = sel ? COLOR_WHITE : (ap->connected ? COLOR_GREEN : COLOR_WHITE);

        /* Selection cursor + signal bars */
        draw_padded_color(2, cy, 2, sel ? "> " : "  ", COLOR_YELLOW, row_bg);
        draw_signal_bars(4, cy, ap->rssi, true, row_bg);

        const char *badge = "";
        if (ap->connected && ap->saved) badge = "[CONNECTED/SAVED]";
        else if (ap->connected)         badge = "[CONNECTED]";
        else if (ap->saved)             badge = "[SAVED]";

        char row_text[120];
        ksnprintf(row_text, sizeof row_text,
                  "  %-32s  %-8s  %2u   %4d dBm  %s",
                  ap->ssid,
                  wifi_auth_name(ap->authmode),
                  ap->channel,
                  (int)ap->rssi,
                  badge);
        draw_padded_color(6, cy, w - 8, row_text, row_fg, row_bg);
    }
}

static void draw_footer(void)
{
    int w  = dq_cols();
    int fy = dq_rows() - FOOTER_ROWS;
    if (s_full_redraw) {
        dq_window(0, fy, w, FOOTER_ROWS);
        draw_padded_color(2, fy + 1, w - 4,
                          "Up/Dn:Select  Enter:Connect  P:Password  D:Disconnect  R:Rescan",
                          COLOR_WHITE, COLOR_BLACK);
        draw_padded_color(2, fy + 2, w - 4,
                          "A:Auto-Connect ON/OFF  N:Manual SSID  F:Forget Saved  Q/ESC:Quit",
                          COLOR_GRAY, COLOR_BLACK);
    }
}

static void redraw_all(void)
{
    wifi_status_t st;
    wifi_get_status(&st);
    draw_header(&st);
    draw_ap_list(&st);
    draw_footer();
    s_full_redraw = false;
}

static void connect_to_selected(bool force_password_prompt)
{
    if (s_ap_count <= 0 || s_sel < 0 || s_sel >= s_ap_count) return;
    wifi_ap_t ap = s_aps[s_sel];

    wifi_status_t cur_st;
    wifi_get_status(&cur_st);
    bool prev_auth_failed = (cur_st.state != WIFI_STATE_CONNECTED &&
                             (cur_st.disconnect_reason == 2u ||
                              cur_st.disconnect_reason == 15u ||
                              cur_st.disconnect_reason == 202u ||
                              cur_st.disconnect_reason == 204u));

    char saved_ssid[WIFI_SSID_MAX + 1] = "";
    char pass[WIFI_PASS_MAX + 1] = "";
    bool auto_c = true;
    bool have_saved_for_ap = false;

    if (wifi_load_saved(saved_ssid, pass, &auto_c) && str_eq(saved_ssid, ap.ssid) && pass[0] != 0) {
        have_saved_for_ap = true;
    } else {
        pass[0] = 0;
    }

    bool prompted = false;
    if (ap.authmode == WIFI_AUTH_OPEN && !force_password_prompt) {
        pass[0] = 0;
    } else if (!have_saved_for_ap || force_password_prompt || prev_auth_failed) {
        char title[80];
        pass[0] = 0;
        ksnprintf(title, sizeof title, "Password for %s (%s):", ap.ssid, wifi_auth_name(ap.authmode));
        if (!dq_input(title, pass, sizeof pass)) {
            s_full_redraw = true;
            return;
        }
        prompted = true;
    }

    char msg[80];
    ksnprintf(msg, sizeof msg, "Connecting to %s ...", ap.ssid);
    show_busy_box(msg, "Saving credentials & requesting DHCP IPv4 address");

    if (!wifi_connect(ap.ssid, pass, true) && !prompted && ap.authmode != WIFI_AUTH_OPEN) {
        char title[80];
        pass[0] = 0;
        ksnprintf(title, sizeof title, "Saved password failed. Enter password for %s:", ap.ssid);
        if (dq_input(title, pass, sizeof pass)) {
            show_busy_box(msg, "Retrying with new password...");
            wifi_connect(ap.ssid, pass, true);
        }
    }
    s_full_redraw = true;
}

static void connect_manual_ssid(void)
{
    char ssid[WIFI_SSID_MAX + 1] = "";
    char pass[WIFI_PASS_MAX + 1] = "";
    if (!dq_input("Enter Wi-Fi SSID:", ssid, sizeof ssid) || !ssid[0]) {
        s_full_redraw = true;
        return;
    }
    if (!dq_input("Enter Wi-Fi Password (blank if Open):", pass, sizeof pass)) {
        s_full_redraw = true;
        return;
    }
    char msg[80];
    ksnprintf(msg, sizeof msg, "Connecting to %s ...", ssid);
    show_busy_box(msg, "Saving credentials & requesting DHCP IPv4 address");
    wifi_connect(ssid, pass, true);
    s_full_redraw = true;
}

bool wifi_app_disconnect_gui(void)
{
    wifi_status_t st;
    wifi_get_status(&st);

    int w = 56;
    if (w > dq_cols() - 4) w = dq_cols() - 4;
    int h = 11;
    int x = (dq_cols() - w) / 2;
    int y = (dq_rows() - h) / 2;

    int choice = 0; /* 0 = Disconnect, 1 = Reconnect/Connect Saved, 2 = Cancel */
    for (;;) {
        wifi_get_status(&st);
        dq_window(x, y, w, h);
        draw_padded_color(x + 2, y + 1, w - 4, "WI-FI CONNECTION CONTROL", COLOR_CYAN, COLOR_BLACK);

        char info1[80], info2[80];
        if (st.state == WIFI_STATE_CONNECTED) {
            ksnprintf(info1, sizeof info1, "Connected AP : %s (%d dBm)", st.ssid, (int)st.rssi);
            ksnprintf(info2, sizeof info2, "IPv4 Address : %u.%u.%u.%u",
                      st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
        } else {
            ksnprintf(info1, sizeof info1, "Current State: DISCONNECTED");
            ksnprintf(info2, sizeof info2, "Saved AP     : %s", st.has_saved ? st.saved_ssid : "(none)");
        }
        draw_padded_color(x + 2, y + 3, w - 4, info1, COLOR_WHITE, COLOR_BLACK);
        draw_padded_color(x + 2, y + 4, w - 4, info2, st.ip[0] ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);

        const char *opts[3] = {
            "Disconnect Wi-Fi now",
            "Connect / Reconnect to saved AP",
            "Close"
        };
        for (int i = 0; i < 3; i++) {
            dq_cursor(x + 3, y + 6 + i, choice == i);
            draw_padded_color(x + 5, y + 6 + i, w - 8, opts[i],
                              choice == i ? COLOR_YELLOW : COLOR_WHITE, COLOR_BLACK);
        }

        int k = dq_getkey();
        if (k == DQK_ESC || k == 'q' || k == 'Q') return false;
        if (k == DQK_UP || k == 'k') choice = (choice + 2) % 3;
        else if (k == DQK_DOWN || k == 'j') choice = (choice + 1) % 3;
        else if (k == 'd' || k == 'D') { choice = 0; k = DQK_ENTER; }
        if (k == DQK_ENTER) {
            if (choice == 0) {
                show_busy_box("Disconnecting Wi-Fi...", "Closing station link on ESP32-C6");
                wifi_disconnect();
                return true;
            } else if (choice == 1) {
                char ssid[WIFI_SSID_MAX + 1] = "";
                char pass[WIFI_PASS_MAX + 1] = "";
                if (wifi_load_saved(ssid, pass, NULL) && ssid[0]) {
                    show_busy_box("Reconnecting to saved AP...", ssid);
                    wifi_connect(ssid, pass, false);
                    return true;
                }
            } else {
                return false;
            }
        }
    }
}

void wifi_app_run(void)
{
    dq_clear();
    s_full_redraw = true;

    show_busy_box("Initializing M5Stack Tab5 Wi-Fi...", "Connecting to ESP32-C6 over SDIO Slot 1");
    wifi_init();
    do_scan();
    redraw_all();

    uint64_t last_refresh = hal_time_now_us();

    for (;;) {
        int k = dq_pollkey();
        if (k == DQK_NONE) {
            if (hal_time_now_us() - last_refresh >= 1000000ULL) {
                wifi_status_t st;
                wifi_get_status(&st);
                draw_header(&st);
                last_refresh = hal_time_now_us();
            }
            task_sleep(30);
            continue;
        }

        int lrows = list_rows();
        switch (k) {
        case 'q': case 'Q': case DQK_ESC:
            return;
        case DQK_DOWN: case 'j':
            if (s_sel + 1 < s_ap_count) s_sel++;
            break;
        case DQK_UP: case 'k':
            if (s_sel > 0) s_sel--;
            break;
        case DQK_PGDN:
            s_sel += lrows;
            if (s_sel >= s_ap_count) s_sel = s_ap_count > 0 ? s_ap_count - 1 : 0;
            break;
        case DQK_PGUP:
            s_sel -= lrows;
            if (s_sel < 0) s_sel = 0;
            break;
        case DQK_HOME:
            s_sel = 0;
            break;
        case DQK_END:
            if (s_ap_count > 0) s_sel = s_ap_count - 1;
            break;
        case DQK_ENTER:
            connect_to_selected(false);
            break;
        case 'p': case 'P':
            connect_to_selected(true);
            break;
        case 'n': case 'N':
            connect_manual_ssid();
            break;
        case 'd': case 'D':
            wifi_app_disconnect_gui();
            s_full_redraw = true;
            break;
        case 'r': case 'R': case 's': case 'S':
            do_scan();
            break;
        case 'a': case 'A': {
            wifi_status_t st;
            wifi_get_status(&st);
            wifi_set_auto_connect(!st.auto_connect);
            break;
        }
        case 'f': case 'F':
            if (dq_confirm("Forget saved Wi-Fi credentials?")) {
                wifi_forget();
            }
            s_full_redraw = true;
            break;
        default:
            break;
        }

        redraw_all();
        last_refresh = hal_time_now_us();
    }
}

void wifi_main(void *arg)
{
    (void)arg;
    fbcon_pause(true);
    task_sleep(100);
    dq_clear();
    wifi_app_run();
    dq_clear();
    fbcon_pause(false);
}
