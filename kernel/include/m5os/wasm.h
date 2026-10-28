#pragma once
#include <m5os/types.h>

/*
 * WebAssembly applications: the sandboxed way to run code somebody else wrote.
 *
 * A .wasm file runs in the wasm3 interpreter inside its own app.  It can only touch the system through the
 * functions the host imports into module "meos" (kernel/app/wasm_host.c, the ABI is in apps/wasm/meos.h), every
 * one of which checks the permissions the app declared; its memory is a bounds-checked linear memory, and everything
 * it allocates is owner-tagged, capped and reclaimed when the app ends.  Unlike M5EX it cannot reach the hardware,
 * so installing a .wasm from a stranger is a much smaller risk than installing machine code.
 *
 * The file carries its own manifest in a custom section "meos.manifest" (text, one key=value per line):
 *     name=hello        1-15 characters of [a-z0-9_-]
 *     perms=CDrw        letters of C D I r w s m, like `run` prints them (see app.h); none = no permissions
 *     mem=512           KB of memory the app may use at most (default 512, up to 8192)
 *     version=1.0       free text
 * A file without a valid manifest is refused.
 */
typedef struct { char name[16]; uint32_t perms; uint32_t mem_kb; char version[16]; } wasm_manifest_t;

/* Reads the manifest of a .wasm image; false if there is none or it is invalid. */
bool wasm_read_manifest(const uint8_t *file, size_t len, wasm_manifest_t *out);

#if M5OS_BOARD_TAB5
int  app_start_wasm_file(const char *path, uint32_t *app_id);
int  app_start_wasm_image(const uint8_t *file, size_t len, uint32_t *app_id);
void wasm_app_cleanup(uint32_t app_id);        /* kernel: an app ended; give back the screen if it held it */

/*
 * Installed apps.  "Installed" = a file /apps/NAME.wasm that also lives in flash (region wasmapps), so it survives a
 * reset; the RAM-disk root is rebuilt from the flash copy at every boot.  Uninstalling removes both.  The firmware
 * ships a few samples (apps/wasm); they are installed on the very first boot only, so removing one sticks.
 */
void wasm_store_boot(void);                                   /* after the root file system is up: restore (and seed) /apps */
int  wasm_install_image(const uint8_t *file, size_t len, char name_out[16]);   /* validate, store in flash and /apps; 0 or -ERR */
int  wasm_install_file(const char *path, char name_out[16]);
int  wasm_uninstall(const char *name);                        /* 0, or ERR_NOENT if there is no such installed app */
bool wasm_is_installed(const char *name);
typedef int (*wasm_list_cb)(const wasm_manifest_t *m, uint32_t size, void *ctx);
int  wasm_list_installed(wasm_list_cb cb, void *ctx);         /* sorted by name; returns the count */
int  wasm_run_installed(const char *name, uint32_t *app_id);  /* start /apps/NAME.wasm */
#else
/* Only the Tab5 has the WebAssembly runtime (it needs PSRAM); the other boards keep their RAM. */
static inline int  app_start_wasm_file(const char *p, uint32_t *id) { (void)p; (void)id; return -4; }
static inline int  app_start_wasm_image(const uint8_t *f, size_t n, uint32_t *id) { (void)f; (void)n; (void)id; return -4; }
static inline void wasm_app_cleanup(uint32_t app_id) { (void)app_id; }
static inline void wasm_store_boot(void) {}
static inline int  wasm_install_file(const char *p, char n[16]) { (void)p; (void)n; return -4; }
static inline int  wasm_uninstall(const char *name) { (void)name; return -13; }
static inline bool wasm_is_installed(const char *name) { (void)name; return false; }
static inline int  wasm_run_installed(const char *name, uint32_t *id) { (void)name; (void)id; return -13; }
#endif
