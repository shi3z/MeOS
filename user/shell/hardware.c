/* Read-only diagnostics. Never initialize cameras, mount cards, scan Wi-Fi or toggle power. */
#include "hardware.h"
#include <m5os/board_config.h>
#include <m5os/kprintf.h>
#if M5OS_BOARD_TAB5
#include <m5os/power.h>
#include <m5os/rootfs.h>
#include <m5os/device.h>
#include <m5os/kmalloc.h>
#include <m5os/wifi.h>
#include <m5os/camera.h>
#include <m5os/errno.h>
#include <m5os/fs.h>
#include <m5os/tab5.h>
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/tab5kbd/tab5touch.h"
#include "../../drivers/tab5kbd/osk.h"

static void row(const char *name, const char *state, const char *detail)
{
    kprintf("%-12s %-16s %s\n", name, state, detail);
}

static int registered(const device_t *dev, void *ctx)
{
    (void)ctx;
    row(dev->name, "registered", "VFS /dev entry; not a hardware health test");
    return 0;
}

void hardware_print_power(void)
{
    power_sample_t p;
    power_read(&p);
    kprintf("INA226 @0x%02x: %s\n", p.address, power_result_name(p.result));
    if (p.result != POWER_NO_RESPONSE && p.result != POWER_UNSUPPORTED)
        kprintf("Identity: manufacturer 0x%04x, die 0x%04x; config 0x%04x\n", p.manufacturer, p.die, p.config);
    if (p.result == POWER_OK) {
        kprintf("Battery rail: %d mV\n", p.battery_mv);
        kprintf("Battery current: %d uA (+ into battery / - out; 5 mOhm shunt)\n", p.battery_ua);
        const char *flow = p.battery_ua > 10000 ? "charging (inferred from current)"
                         : p.battery_ua < -10000 ? "discharging (battery contributes)"
                         : "near zero / indeterminate";
        kprintf("Flow: %s\n", flow);
        if (p.estimated_percent >= 0)
            kprintf("Level: ~%d%% (coarse 2S voltage estimate; load/charging affect it)\n", p.estimated_percent);
        else kputs("Level: unknown (no plausible 2S pack voltage; battery presence unknown)\n");
    } else kputs("Battery voltage/current/level: unavailable (no fresh sample)\n");
    if (p.charger_valid) {
        kprintf("Charge permission: %s (configuration only)\n", p.charge_enabled ? "enabled" : "disabled");
        kprintf("CHG_STAT: %s (raw input; not proof of external power)\n", p.charge_status_high ? "high" : "low");
    } else kputs("Charger expander: read error or unexpected pin configuration\n");
    kputs("Power source: unknown (USB-C / external DC / battery not independently detected)\n");
    kputs("Read-only: charger and INA226 configuration are unchanged.\n");
}

void hardware_print_storage(void)
{
    kputs(rootfs_is_persistent() ? "Root /: M5FS on SD (persistent)\n" : "Root /: M5FS on RAM disk (volatile; files lost at reset)\n");
    fs_stat_t s;
    if (fs_statfs(&s) == ERR_OK)
        kprintf("Root capacity: %u KB, free: %u KB\n", s.total_blocks * s.block_size / 1024u, s.free_blocks * s.block_size / 1024u);
    else kputs("Root capacity: read error\n");
    kputs("microSD: unsupported on Tab5; insertion/capacity/content unknown\n");
    kputs("FAT/exFAT: not implemented; no card is probed, mounted or formatted\n");
    kputs("WASM apps: separate flash store, restored to /apps at boot\n");
}

void hardware_print_devices(void)
{
    kputs("DEVICE       STATE            DETAIL\n");
    device_foreach(registered, NULL);
    kmem_stats_t m;
    kmem_psram_stats(&m);
    kprintf("%-12s %-16s %u KB available to MeOS\n", "PSRAM", m.total ? "ready" : "unavailable", m.total / 1024u);
    const char *panel = tab5_panel() == TAB5_PANEL_ST7121 ? "ST7121" :
                        tab5_panel() == TAB5_PANEL_ILI9881C ? "ILI9881C" :
                        tab5_panel() == TAB5_PANEL_ST7123 ? "ST7123 (LCD init unsupported)" : "unknown";
    row("panel", "board selection", panel);
    row("touch", osk_available() ? "detected" : "not detected", tab5_touch_name());
    row("keyboard", "driver reports", keyboard_type());
    wifi_status_t w;
    wifi_get_status(&w);
    row("Wi-Fi", w.hw_ready ? "initialized" : "not ready", wifi_state_name(w.state));
    camera_status_t c;
    camera_get_status(&c);
    row("camera", c.initialized ? "initialized" : "not tested", "SC202CS driver; sensor variant not probed here");
    power_sample_t p;
    power_read(&p);
    row("INA226", power_result_name(p.result), "system I2C 0x41; details: power");
    row("microSD", "unsupported", "Tab5 SDMMC driver absent; details: storage");
    row("RTC", "unsupported", "RX8130CE; presence not probed");
    row("IMU", "unsupported", "BMI270; presence not probed");
    row("audio", "unsupported", "codec/microphones/speaker; presence not probed");
    row("RS485", "unsupported", "no serial app or transceiver control");
    row("USB host", "keyboard only", "no generic mass-storage/mouse support");
    kputs("unsupported != absent. Only INA226/charger are read on demand; other rows report existing state.\n");
}
#endif
