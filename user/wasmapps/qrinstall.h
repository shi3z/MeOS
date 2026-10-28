#pragma once
#include <m5os/types.h>
/* Camera -> QR code -> download -> confirm -> install a WebAssembly app.  Full-screen; the caller owns the screen
 * (fbcon paused).  Returns true if an app was installed (its name in installed_name, 16 bytes, may be NULL). */
bool qrinstall_run(char *installed_name);
/* The same from the text of a code (the shell: `install https://...`), no camera. */
bool qrinstall_code(const char *code, char *installed_name);
