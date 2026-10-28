#pragma once
#include <m5os/types.h>

/* Launch the interactive graphical Web Browser on the LCD */
void web_app_run(void);

/* Launch the Web Browser at a specific initial URL */
void web_app_open_url(const char *initial_url);

/* Shell CLI entry point: `web [url]` or `web --dump <url>` */
int web_cmd_main(int argc, char **argv);

/* Download a URL into `buf` (body only, http/https, follows redirects). false + message in `err` on failure. */
bool web_download(const char *url, uint8_t *buf, size_t cap, size_t *out_len, char *err, size_t err_cap);
