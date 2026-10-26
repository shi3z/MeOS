#include "apps.h"
#include <m5os/quake.h>
#include <m5os/quake_video.h>
#include <m5os/doom.h>

void hello_main(void *arg);
void ticker_main(void *arg);
void hog_main(void *arg);
void rogue_main(void *arg);
void leaker_main(void *arg);
void family_main(void *arg);
void notes_main(void *arg);
void echod_main(void *arg);
void launcher_main(void *arg);
void terminal_main(void *arg);
void clock_main(void *arg);
void calculator_main(void *arg);
void snake_main(void *arg);
void intruder_main(void *arg);
void camera_main(void *arg);
void wifi_main(void *arg);
void ssh_main(void *arg);
void web_main(void *arg);

#define APP_STACK_DEFAULT  4096u
#define APP_STACK_GUI      8192u
#define APP_STACK_CAMERA   24576u
#define APP_PRIO_DEFAULT   TASK_PRIO_DEFAULT

/* name, entry, permissions, stack, priority, description */
static const app_def_t s_defs[] = {
#if M5OS_QUAKE
    { "quake",quake_main,APP_PERM_CONSOLE|APP_PERM_DISPLAY|APP_PERM_FS_READ|APP_PERM_SPAWN,49152u,APP_PRIO_DEFAULT,"experimental Quake " QV_MODE_NAME "; effects, no network/save" },
#endif
#if M5OS_DOOM
    { "doom", doom_main, APP_PERM_CONSOLE | APP_PERM_DISPLAY | APP_PERM_FS_READ | APP_PERM_SPAWN,
                        32768u, APP_PRIO_DEFAULT, "native Doom, audio 320x200; touch controls; Q quits" },
#endif
    { "hello",  hello_main,  APP_PERM_CONSOLE, APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "print a greeting and exit" },
    { "ticker", ticker_main, APP_PERM_CONSOLE, APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "one line per second, five times" },
    { "hog",    hog_main,    0,                APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "burn CPU forever (stop it with: stop ID)" },
    { "notes",  notes_main,  APP_PERM_CONSOLE | APP_PERM_FS_READ | APP_PERM_FS_WRITE,
                                               APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "append a line to /home/notes.txt and show the file" },
    { "echod",  echod_main,  APP_PERM_CONSOLE | APP_PERM_IPC,
                                               APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "echo server: send it messages (shell: send PID TEXT, recv)" },
    { "rogue",  rogue_main,  APP_PERM_CONSOLE, APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "tries display/file-write/spawn/messages without permission" },
    { "leaker", leaker_main, APP_PERM_CONSOLE, APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "leaks memory and handles; the kernel takes them back" },
    { "family", family_main, APP_PERM_CONSOLE | APP_PERM_SPAWN,
                                               APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "spawns a child task; stopping the app stops both" },
    { "intruder",  intruder_main,   APP_PERM_DISPLAY, APP_STACK_DEFAULT, APP_PRIO_DEFAULT, "tries to use another app's window" },
    { "camera",    camera_main,     APP_PERM_CONSOLE | APP_PERM_DISPLAY | APP_PERM_FS_READ | APP_PERM_FS_WRITE,
                                               APP_STACK_CAMERA, APP_PRIO_DEFAULT, "camera viewfinder & BMP capture (/home/cam_XXXX.bmp)" },
    { "wifi",      wifi_main,       APP_PERM_CONSOLE | APP_PERM_DISPLAY | APP_PERM_FS_READ | APP_PERM_FS_WRITE,
                                               APP_STACK_CAMERA, APP_PRIO_DEFAULT, "graphical Wi-Fi scanner & connection manager" },
    { "ssh",       ssh_main,        APP_PERM_CONSOLE | APP_PERM_DISPLAY | APP_PERM_FS_READ | APP_PERM_FS_WRITE,
                                               APP_STACK_CAMERA, APP_PRIO_DEFAULT, "SSH-2 remote shell client (curve25519-sha256)" },
    { "web",       web_main,        APP_PERM_CONSOLE | APP_PERM_DISPLAY | APP_PERM_FS_READ | APP_PERM_FS_WRITE,
                                               APP_STACK_CAMERA, APP_PRIO_DEFAULT, "HTTP/HTTPS UTF-8 Japanese Web Browser" },
    /* ---- GUI apps: D = windows, m = receive events; the launcher also starts apps (s) ---- */
    { "launcher",   launcher_main,   APP_PERM_DISPLAY | APP_PERM_IPC | APP_PERM_SPAWN,
                                               APP_STACK_GUI, APP_PRIO_DEFAULT, "home screen" },
    { "terminal",   terminal_main,   APP_PERM_CONSOLE | APP_PERM_DISPLAY | APP_PERM_IPC,
                                               APP_STACK_GUI, APP_PRIO_DEFAULT, "console view with command buttons" },
    { "clock",      clock_main,      APP_PERM_DISPLAY | APP_PERM_IPC, APP_STACK_GUI, APP_PRIO_DEFAULT, "digital + analog clock (since boot)" },
    { "calculator", calculator_main, APP_PERM_DISPLAY | APP_PERM_IPC, APP_STACK_GUI, APP_PRIO_DEFAULT, "integer calculator" },
    { "snake",      snake_main,      APP_PERM_DISPLAY | APP_PERM_IPC, APP_STACK_GUI, APP_PRIO_DEFAULT, "snake game (touch to steer)" },
};

const app_def_t *apps_def_table(int *count)
{
    *count = (int)(sizeof s_defs / sizeof s_defs[0]);
    return s_defs;
}
