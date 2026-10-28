/*
 * The large SKK dictionary lives in its own flash region (tools/ime_flash_dict.sh writes it); it is far too big
 * for the application partition.  It is copied to PSRAM by a background task right after boot, a few KB at a
 * time, so the keyboard and the shell stay responsive; until it is there the built-in mini dictionary serves.
 *
 * The flash is read with the ROM routine and interrupts off for each small chunk, like the SSH key store.
 */
#include <m5os/kprintf.h>
#include <m5os/kmalloc.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include "ime_dict.h"
#include "esp_attr.h"
#include "esp_rom_spiflash.h"

#define DICT_FLASH_ADDR  0x00400000u            /* partitions-tab5.csv: "imedict" */
#define DICT_FLASH_MAX   0x00600000u
#define CHUNK            2048u
#define CHUNKS_PER_NAP   4

static bool IRAM_ATTR flash_read(uint32_t addr, void *dst, uint32_t n)
{
    uint32_t f = arch_irq_disable();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_read(addr, (uint32_t *)dst, n);
    arch_irq_restore(f);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}

static void loader_task(void *arg)
{
    (void)arg;
    uint32_t head[4] __attribute__((aligned(4)));
    if (!flash_read(DICT_FLASH_ADDR, head, sizeof head)) return;
    uint32_t total = head[2];
    if (head[0] != IME_DICT_MAGIC || total < IME_DICT_HEADER || total > DICT_FLASH_MAX) {
        kprintf("ime: no large dictionary in flash (tools/ime_flash_dict.sh), using the built-in one\n");
        return;
    }
    uint32_t alloc = (total + 15u) & ~15u;
    uint8_t *buf = kmalloc_psram(alloc);
    if (!buf) { kprintf("ime: no PSRAM for the dictionary (%u bytes)\n", (unsigned)total); return; }
    for (uint32_t off = 0; off < alloc; off += CHUNK) {
        uint32_t n = alloc - off < CHUNK ? alloc - off : CHUNK;
        if (!flash_read(DICT_FLASH_ADDR + off, buf + off, (n + 3u) & ~3u)) { kprintf("ime: dictionary read failed at %u\n", (unsigned)off); kfree(buf); return; }
        if ((off / CHUNK) % CHUNKS_PER_NAP == CHUNKS_PER_NAP - 1) task_sleep(1);
    }
    if (ime_dict_add(buf, total)) kprintf("ime: dictionary loaded (%u KB)\n", (unsigned)(total / 1024u));
    else { kprintf("ime: dictionary in flash is damaged\n"); kfree(buf); }
}

void ime_dict_load_async(void)
{
    task_create_stack("imedict", loader_task, 0, TASK_PRIO_DEFAULT, 4096);
}
