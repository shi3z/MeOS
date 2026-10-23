#include <m5os/app_cleanup.h>
#include <m5os/board_config.h>
#include <m5os/errno.h>
#include <m5os/kernel.h>
#include <m5os/gui.h>
#include <m5os/vfs.h>
#include <m5os/wasm.h>
#if M5OS_BOARD_TAB5
#include <m5os/audio.h>
#include <m5os/microphone.h>
static int stop_microphone(uint32_t id) { return microphone_app_cleanup(id); }
static int stop_audio(uint32_t id) { audio_close(id); return ERR_OK; }
#endif
static int close_windows(uint32_t id) { gui_release_owner(id); return ERR_OK; }
static int close_files(uint32_t id) { vfs_close_owned(id); return ERR_OK; }
static int close_wasm(uint32_t id) { wasm_app_cleanup(id); return ERR_OK; }
static void add(const char *name,app_cleanup_fn fn) {
    if(app_cleanup_register(name,fn)!=ERR_OK) kernel_panic("cleanup registration failed");
}
void app_cleanup_register_builtins(void)
{
#if M5OS_BOARD_TAB5
    add("microphone",stop_microphone); add("audio",stop_audio);
#endif
    add("windows",close_windows); add("files",close_files); add("wasm",close_wasm);
    app_cleanup_seal();
}
