#pragma once
#include <m5os/types.h>

/* Launch the full-screen graphical Wi-Fi scanner & configuration app. */
void wifi_app_run(void);

/* Launch a graphical Wi-Fi disconnect / connection control modal from the CLI. */
bool wifi_app_disconnect_gui(void);

/* Built-in app entry point for `run wifi`. */
void wifi_main(void *arg);
