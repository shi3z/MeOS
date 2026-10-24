/* SD/SDSC/SDHC/SDXC SPI transport. Single-block CMD24 writes wait for
 * programming completion and check CMD13 status. No erase command is used. */
#include <m5os/sdcard.h>
#include <m5os/hal_sdspi.h>
#include <m5os/hal_gpio.h>
#include <m5os/hal_time.h>
#include <m5os/bus.h>
#include <m5os/errno.h>

static sdcard_status_t s = { .detail = "not probed" };
static blockdev_t s_bd;
static uint32_t now_ms(void) { return (uint32_t)(hal_time_now_us() / 1000u); }
static uint16_t crc16(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    return crc;
}
static void deselect(void) { hal_sdspi_cs(false); hal_sdspi_xfer(0xff); }
static bool wait_ready(void)
{
    uint32_t start = now_ms();
    do { if (hal_sdspi_xfer(0xff) == 0xff) return true; } while (now_ms() - start < 500);
    return false;
}
static uint8_t command(uint8_t cmd, uint32_t arg, uint8_t crc)
{
    if (cmd != 0 && !wait_ready()) return 0xff;
    hal_sdspi_xfer(0x40 | cmd);
    for (int i = 24; i >= 0; i -= 8) hal_sdspi_xfer((uint8_t)(arg >> i));
    hal_sdspi_xfer(crc);
    uint8_t r = 0xff;
    for (int i = 0; i < 10; i++) { r = hal_sdspi_xfer(0xff); if (!(r & 0x80)) break; }
    s.last_r1 = r;
    return r;
}
static bool data(uint8_t *buf, size_t len)
{
    uint32_t start = now_ms();
    uint8_t token;
    do { token = hal_sdspi_xfer(0xff); } while (token == 0xff && now_ms() - start < 200);
    if (token != 0xfe) return false;
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) crc = crc16(crc, buf[i] = hal_sdspi_xfer(0xff));
    uint16_t received = (uint16_t)hal_sdspi_xfer(0xff) << 8;
    received |= hal_sdspi_xfer(0xff);
    return received == crc;
}
static uint32_t capacity(const uint8_t *c)
{
    uint64_t blocks;
    if ((c[0] >> 6) == 1) {
        uint32_t size = ((uint32_t)(c[7] & 0x3f) << 16) | ((uint32_t)c[8] << 8) | c[9];
        blocks = ((uint64_t)size + 1) * 1024;
    } else if ((c[0] >> 6) == 0) {
        unsigned shift = (c[5] & 15) + (((c[9] & 3) << 1) | (c[10] >> 7)) + 2;
        uint32_t size = ((uint32_t)(c[6] & 3) << 10) | ((uint32_t)c[7] << 2) | (c[8] >> 6);
        if ((c[5] & 15) < 9 || (c[5] & 15) > 11) return 0;
        blocks = ((uint64_t)size + 1) << shift;
        blocks /= 512;
    } else return 0;
    return blocks && blocks <= UINT32_MAX ? (uint32_t)blocks : 0;
}
static int read_blocks(blockdev_t *bd, uint32_t lba, uint32_t count, void *buf)
{
    (void)bd;
    spi_bus_lock();
    int rc = ERR_OK;
    if (!s.ready) { rc = ERR_NODEV; goto out; }
    if (!buf || !count || lba >= s.sectors || count > s.sectors - lba) { rc = ERR_INVAL; goto out; }
    hal_sdspi_begin(); hal_sdspi_cs(true);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t block = lba + i;
        if (!s.high_capacity && block > UINT32_MAX / 512u) { rc = ERR_INVAL; break; }
        if (command(17, s.high_capacity ? block : block * 512u, 1) != 0 ||
            !data((uint8_t *)buf + (size_t)i * 512, 512)) {
            s.ready = false; s.detail = "read/CRC error; unmount then probe again";
            rc = ERR_IO; break;
        }
    }
    deselect(); hal_sdspi_end();
out:
    spi_bus_unlock(); return rc;
}
static int write_blocks(blockdev_t *bd, uint32_t lba, uint32_t count, const void *buf)
{
    (void)bd;
    spi_bus_lock();
    int rc = ERR_OK;
    if (!s.ready) { rc = ERR_NODEV; goto out; }
    if (s.write_protected) { rc = ERR_PERM; goto out; }
    if (!buf || !count || lba >= s.sectors || count > s.sectors-lba) { rc = ERR_INVAL; goto out; }
    hal_sdspi_begin(); hal_sdspi_cs(true);
    for (uint32_t i=0; i<count; i++) {
        uint32_t block=lba+i;
        if (!s.high_capacity && block>UINT32_MAX/512u) { rc=ERR_INVAL; break; }
        if (command(24, s.high_capacity ? block : block*512u, 1) != 0) { rc=ERR_IO; break; }
        hal_sdspi_xfer(0xff); hal_sdspi_xfer(0xfe);
        const uint8_t *bytes=(const uint8_t *)buf+(size_t)i*512;
        uint16_t crc=0;
        for (unsigned j=0;j<512;j++) { hal_sdspi_xfer(bytes[j]); crc=crc16(crc,bytes[j]); }
        hal_sdspi_xfer((uint8_t)(crc>>8)); hal_sdspi_xfer((uint8_t)crc);
        uint8_t response=0xff;
        for (unsigned n=0;n<10 && response==0xff;n++) response=hal_sdspi_xfer(0xff);
        if ((response&31)!=5 || !wait_ready()) { rc=ERR_IO; break; }
        if (command(13,0,1)!=0 || hal_sdspi_xfer(0xff)!=0) { rc=ERR_IO; break; }
    }
    if (rc==ERR_IO) { s.ready=false; s.detail="write/status error; unmount then probe again"; }
    deselect(); hal_sdspi_end();
out:
    spi_bus_unlock(); return rc;
}

blockdev_t *sdcard_probe(void)
{
    spi_bus_lock();
    s = (sdcard_status_t){ .probed = true, .last_r1 = 0xff, .detail = "slot power failed" };
    if (!sdcard_spi_power_init()) { spi_bus_unlock(); return NULL; }
    hal_sdspi_begin(); hal_sdspi_set_slow(true); hal_sdspi_cs(false);
    for (int i = 0; i < 10; i++) hal_sdspi_xfer(0xff);
    hal_sdspi_cs(true);
    bool ok = false, v2 = false;
    uint8_t r = 0xff, csd[16];
    for (int i = 0; i < 5 && r != 1; i++) r = command(0, 0, 0x95);
    s.detail = "no response or card fault (no card-detect pin)";
    if (r != 1) goto out;
    s.detail = "CMD8 error";
    r = command(8, 0x1aa, 0x87);
    if (r == 1) {
        uint32_t echo = 0;
        for (int i = 0; i < 4; i++) echo = (echo << 8) | hal_sdspi_xfer(0xff);
        if (echo != 0x1aa) goto out;
        v2 = true;
    } else if (r != 5) goto out;
    s.detail = "initialization timeout/error";
    uint32_t start = now_ms();
    do {
        if (command(55, 0, 1) > 1) goto out;
        r = command(41, v2 ? 0x40000000u : 0, 1);
        if (r > 1) goto out;
        if (r == 0) break;
        hal_delay_us(1000);
    } while (now_ms() - start < 1500);
    if (r != 0) goto out;
    s.detail = "OCR voltage/power error";
    if (command(58, 0, 1) != 0) goto out;
    uint32_t ocr = 0;
    for (int i = 0; i < 4; i++) ocr = (ocr << 8) | hal_sdspi_xfer(0xff);
    if (!(ocr & 0x80000000u) || !(ocr & 0x00ff8000u)) goto out;
    s.high_capacity = v2 && (ocr & 0x40000000u);
    if (!s.high_capacity && command(16, 512, 1) != 0) { s.detail = "block length error"; goto out; }
    hal_sdspi_set_slow(false);
    s.detail = "CSD read/CRC error";
    if (command(9, 0, 1) != 0 || !data(csd, sizeof csd)) goto out;
    s.detail = "unsupported capacity/CSD";
    if (((csd[0] >> 6) == 1) != s.high_capacity) goto out;
    s.sectors = capacity(csd);
    if (!s.sectors) goto out;
    s.write_protected = (csd[14] & 0x30) != 0;
    s.ready = true; s.detail = s.write_protected ? "ready (SPI, card write-protected)" : "ready (SPI, read/write)"; ok = true;
out:
    deselect(); hal_sdspi_end(); spi_bus_unlock();
    if (!ok) return NULL;
    s_bd = (blockdev_t){ .name = "sd0", .block_size = 512, .block_count = s.sectors,
                        .read = read_blocks, .write = write_blocks };
    return &s_bd;
}
void sdcard_status(sdcard_status_t *out) { spi_bus_lock(); *out = s; spi_bus_unlock(); }
