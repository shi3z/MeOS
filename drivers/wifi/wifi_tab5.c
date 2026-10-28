/*
 * M5Stack Tab5 Wi-Fi driver (FreeRTOS-free)
 *
 * Architecture:
 *  - Host: ESP32-P4 SDMMC Controller Slot 1 (4-bit SDIO @ 20 MHz, polled DMA)
 *    Pins: CLK=GPIO12, CMD=GPIO13, D0=GPIO11, D1=GPIO10, D2=GPIO9, D3=GPIO8, RST=GPIO15
 *  - Co-processor: Onboard ESP32-C6-MINI-1U running ESP-Hosted-MCU slave firmware (v0.0.6)
 *  - Transport: ESP-Hosted SDIO streaming framing (12-byte esp_payload_header)
 *  - Control plane: Protobuf RPC over ESP_SERIAL_IF (if_type=3, TLV endpoint "RPCRsp"/"RPCEvt")
 *  - Data plane: Ethernet II frames over ESP_STA_IF (if_type=0) with built-in DHCPv4, ARP & ICMP responder
 *  - Credential storage: Persisted both in /home/wifi.cfg (M5FS) and ESP32-C6 NVS flash
 */
#include <m5os/wifi.h>
#include <m5os/kprintf.h>
#include <m5os/hal_time.h>
#include <m5os/task.h>
#include <m5os/mutex.h>
#include <m5os/vfs.h>
#include <m5os/kmalloc.h>
#include <m5os/errno.h>

#include "soc/sdmmc_struct.h"
#include "soc/sdmmc_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal/sdmmc_ll.h"
#include "hal/gpio_ll.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_cache.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"

/* Satisfy HAL atomic macro check in FreeRTOS-free kernel */
static int __DECLARE_RCC_ATOMIC_ENV;

/* =========================================================================
 * Pin & SDIO Register Definitions
 * ========================================================================= */

#define SDIO_SLOT               1
#define PIN_SDIO_CLK            12
#define PIN_SDIO_CMD            13
#define PIN_SDIO_D0             11
#define PIN_SDIO_D1             10
#define PIN_SDIO_D2             9
#define PIN_SDIO_D3             8
#define PIN_C6_RESET            15

#define SDIO_BLOCK_SIZE         512u
#define SDIO_REG(x)             ((x) & 0x3FFu)

/* Standard SDIO Function 0 (CCCR / FBR) registers */
#define CCCR_FN_ENABLE          0x02u
#define CCCR_FN_READY           0x03u
#define CCCR_INT_ENABLE         0x04u
#define CCCR_BUS_WIDTH          0x07u
#define CCCR_BLKSIZEL           0x10u
#define CCCR_BLKSIZEH           0x11u
#define CCCR_HIGHSPEED          0x13u
#define FBR1_BLKSIZEL           0x110u
#define FBR1_BLKSIZEH           0x111u

/* ESP32-C6 SDIO Slave Function 1 registers */
#define ESP_SLAVE_TOKEN_RDATA   SDIO_REG(0x044u)
#define ESP_SLAVE_INT_RAW_REG   SDIO_REG(0x050u)
#define ESP_SLAVE_PKT_LEN_REG   SDIO_REG(0x060u)
#define ESP_SLAVE_SCRATCH_REG_7 SDIO_REG(0x08Cu)
#define ESP_SLAVE_INT_CLR_REG   SDIO_REG(0x0D4u)
#define ESP_SLAVE_CMD53_END     0x1F800u
#define ESP_OPEN_DATAPATH       0x01u

/* ESP-Hosted interface types (esp_hosted_interface.h: INVALID=0, STA=1, AP=2, SERIAL=3, HCI=4, PRIV=5) */
#define ESP_STA_IF              1u
#define ESP_AP_IF               2u
#define ESP_SERIAL_IF           3u
#define ESP_HCI_IF              4u
#define ESP_PRIV_IF             5u

/* ESP-Hosted RPC constants */
#define RPC_TYPE_REQ            1u
#define RPC_TYPE_RESP           2u
#define RPC_TYPE_EVENT          3u

#define RPC_REQ_WIFI_GET_MAC            257u
#define RPC_REQ_WIFI_SET_MODE           260u
#define RPC_REQ_WIFI_SET_PS             270u
#define RPC_REQ_WIFI_INIT               278u
#define RPC_REQ_WIFI_START              280u
#define RPC_REQ_WIFI_STOP               281u
#define RPC_REQ_WIFI_CONNECT            282u
#define RPC_REQ_WIFI_DISCONNECT         283u
#define RPC_REQ_WIFI_SET_CONFIG         284u
#define RPC_REQ_WIFI_GET_CONFIG         285u
#define RPC_REQ_WIFI_SCAN_START         286u
#define RPC_REQ_WIFI_SCAN_GET_AP_NUM    288u
#define RPC_REQ_WIFI_SCAN_GET_AP_RECS   289u
#define RPC_REQ_WIFI_STA_GET_AP_INFO    294u
#define RPC_REQ_WIFI_SET_STORAGE        313u
#define RPC_REQ_GET_COPROC_FW_VER       350u

#define RPC_RESP_WIFI_GET_MAC           513u
#define RPC_RESP_WIFI_SET_MODE          516u
#define RPC_RESP_WIFI_SET_PS            526u
#define RPC_RESP_WIFI_INIT              534u
#define RPC_RESP_WIFI_START             536u
#define RPC_RESP_WIFI_STOP              537u
#define RPC_RESP_WIFI_CONNECT           538u
#define RPC_RESP_WIFI_DISCONNECT        539u
#define RPC_RESP_WIFI_SET_CONFIG        540u
#define RPC_RESP_WIFI_GET_CONFIG        541u
#define RPC_RESP_WIFI_SCAN_START        542u
#define RPC_RESP_WIFI_SCAN_GET_AP_NUM   544u
#define RPC_RESP_WIFI_SCAN_GET_AP_RECS  545u
#define RPC_RESP_WIFI_STA_GET_AP_INFO   550u
#define RPC_RESP_WIFI_SET_STORAGE       569u
#define RPC_RESP_GET_COPROC_FW_VER      606u

#define RPC_EVT_WIFI_NO_ARGS            773u
#define RPC_EVT_STA_SCAN_DONE           774u
#define RPC_EVT_STA_CONNECTED           775u
#define RPC_EVT_STA_DISCONNECTED        776u

/* =========================================================================
 * Static Driver State & Aligned DMA Buffers
 * ========================================================================= */

static sdmmc_desc_t s_dma_desc[16] __attribute__((aligned(64)));
static uint8_t      s_dma_buf[8192] __attribute__((aligned(64)));
static uint8_t      s_rx_packet_buf[8192];
static uint8_t      s_tx_packet_buf[2048];

static mutex_t      s_wifi_lock;
static bool         s_lock_ready;
static bool         s_sdio_ready;
static bool         s_priv_init_done;
static bool         s_wifi_inited;
static bool         s_daemon_started;

static uint32_t     s_sdio_rx_bytes;
static uint32_t     s_sdio_tx_bufs;
static uint16_t     s_seq_num;
static uint32_t     s_rpc_uid;

/* Pending synchronous RPC response state */
static volatile uint32_t s_wait_msg_id;
static volatile uint32_t s_wait_uid;
static volatile bool     s_wait_done;
static int32_t           s_wait_resp_code;
static uint8_t           s_wait_payload[4096];
static size_t            s_wait_payload_len;

/* Scan results buffer populated during RPC_RESP_WIFI_SCAN_GET_AP_RECS */
static wifi_ap_t    s_scan_aps[WIFI_MAX_SCAN_APS];
static int          s_scan_count;

/* Current station status */
static wifi_status_t s_status;
static char          s_saved_pass[WIFI_PASS_MAX + 1];
static bool          s_manual_disconnect;

/* Lightweight DHCP client state */
static uint32_t     s_dhcp_xid = 0x54414235u;
static uint8_t      s_dhcp_state; /* 0=idle, 1=discover_sent, 2=request_sent, 3=bound */
static uint64_t     s_dhcp_last_tx_us;
static uint8_t      s_dhcp_offered_ip[4];
static uint8_t      s_dhcp_server_ip[4];
static uint8_t      s_dns_ip[4] = {8, 8, 8, 8};

/* ARP cache */
#define ARP_CACHE_SIZE 8
typedef struct {
    bool    valid;
    uint8_t ip[4];
    uint8_t mac[6];
} arp_entry_t;
static arp_entry_t  s_arp_cache[ARP_CACHE_SIZE];
static uint8_t      s_arp_next_slot;

/* DNS client state */
static uint16_t     s_dns_txid = 0x1234u;
static volatile bool s_dns_done;
static uint8_t      s_dns_resolved_ip[4];

/* Multi-connection TCP client state */
#define TCP_STATE_CLOSED      0
#define TCP_STATE_SYN_SENT    1
#define TCP_STATE_ESTABLISHED 2
#define TCP_STATE_CLOSE_WAIT  3

#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u

#define TCP_RX_BUF_SIZE 65536u

typedef struct {
    volatile uint8_t state;
    uint8_t      remote_ip[4];
    uint8_t      remote_mac[6];
    uint16_t     local_port;
    uint16_t     remote_port;
    uint32_t     snd_nxt;
    uint32_t     snd_una;
    uint32_t     rcv_nxt;
    uint8_t     *rx_buf;
    size_t       rx_head;
    size_t       rx_tail;
    size_t       rx_count;
} tcp_conn_t;

static tcp_conn_t   s_tcp_conns[WIFI_MAX_TCP_SOCKETS];
static uint16_t     s_next_local_port = 49152u;
static uint16_t     s_ip_id = 1u;

/* UDP rx callback & Tailscale virtual tunnel (100.64.0.0/10) state */
static uint16_t              s_udp_rx_port;
static wifi_udp_rx_cb_t      s_udp_rx_cb;
static uint8_t               s_tun_ip[4];
static wifi_tun_tx_cb_t      s_tun_tx_cb;
static wifi_tun_resolve_cb_t s_tun_resolve_cb;

static bool is_tailscale_ip(const uint8_t ip[4])
{
    return ip && ip[0] == 100u && (ip[1] & 0xC0u) == 64u;
}


/* =========================================================================
 * String / Byte Helpers
 * ========================================================================= */

static void mem_zero(void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void mem_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static size_t str_len(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, size_t max, const char *src)
{
    if (!max) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && i < max - 1; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

static bool str_eq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const char *wifi_auth_name(uint8_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
    case WIFI_AUTH_ENTERPRISE:    return "EAP";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK:      return "WAPI";
    case WIFI_AUTH_OWE:           return "OWE";
    default:                      return "WPA2";
    }
}

const char *wifi_state_name(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_UNINIT:       return "UNINIT";
    case WIFI_STATE_DISCONNECTED: return "DISCONNECTED";
    case WIFI_STATE_SCANNING:     return "SCANNING";
    case WIFI_STATE_CONNECTING:   return "CONNECTING";
    case WIFI_STATE_CONNECTED:    return "CONNECTED";
    case WIFI_STATE_ERROR:        return "ERROR";
    default:                      return "UNKNOWN";
    }
}

static void set_msg(const char *msg)
{
    str_copy(s_status.last_msg, sizeof s_status.last_msg, msg);
}

/* =========================================================================
 * Low-level Polled SDMMC Controller (Slot 1)
 * ========================================================================= */

static void gpio_setup_sdio_pin(int pin, int in_sig, int out_sig, bool pullup)
{
    gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO);
    gpio_ll_input_enable(&GPIO, pin);
    if (pullup) {
        gpio_ll_pullup_en(&GPIO, pin);
        gpio_ll_pulldown_dis(&GPIO, pin);
    } else {
        gpio_ll_pullup_dis(&GPIO, pin);
        gpio_ll_pulldown_dis(&GPIO, pin);
    }
    gpio_ll_set_drive_capability(&GPIO, pin, GPIO_DRIVE_CAP_3);
    if (out_sig >= 0) {
        esp_rom_gpio_connect_out_signal(pin, out_sig, false, false);
    }
    if (in_sig >= 0) {
        esp_rom_gpio_connect_in_signal(pin, in_sig, false);
    }
}

static bool sdmmc_update_clock(void)
{
    sdmmc_hw_cmd_t cmd;
    mem_zero(&cmd, sizeof cmd);
    cmd.card_num = SDIO_SLOT;
    cmd.update_clk_reg = 1;
    cmd.wait_complete = 1;
    cmd.use_hold_reg = 1;
    cmd.start_command = 1;

    for (int tries = 0; tries < 200; tries++) {
        SDMMC.rintsts.val = 0xFFFFFFFFu;
        SDMMC.cmd = cmd;
        for (int poll = 0; poll < 20000; poll++) {
            if (SDMMC.rintsts.hle) break;
            if (!SDMMC.cmd.start_command) {
                SDMMC.rintsts.val = 0xFFFFFFFFu;
                return true;
            }
            esp_rom_delay_us(1);
        }
        esp_rom_delay_us(10);
    }
    return false;
}

static bool sdmmc_set_clock_khz(uint32_t freq_khz)
{
    sdmmc_ll_enable_card_clock(&SDMMC, SDIO_SLOT, false);
    if (!sdmmc_update_clock()) return false;

    uint32_t host_div = 10;
    uint32_t card_div = 20; /* 160 MHz / 10 / (2 * 20) = 400 kHz */
    if (freq_khz >= 40000u) {
        host_div = 4;
        card_div = 0;       /* 160 MHz / 4 = 40 MHz */
    } else if (freq_khz >= 20000u) {
        host_div = 8;
        card_div = 0;       /* 160 MHz / 8 = 20 MHz */
    }

    sdmmc_ll_set_card_clock_div(&SDMMC, SDIO_SLOT, card_div);
    sdmmc_ll_set_clock_div(&SDMMC, host_div);
    sdmmc_ll_init_phase_delay(&SDMMC);
    if (!sdmmc_update_clock()) return false;

    sdmmc_ll_enable_card_clock(&SDMMC, SDIO_SLOT, true);
    sdmmc_ll_enable_card_clock_low_power(&SDMMC, SDIO_SLOT, true);
    return sdmmc_update_clock();
}

static bool sdmmc_wait_idle(uint32_t timeout_us)
{
    for (uint32_t t = 0; t < timeout_us; t += 2u) {
        if (!SDMMC.cmd.start_command && !sdmmc_ll_is_card_data_busy(&SDMMC)) {
            return true;
        }
        esp_rom_delay_us(2);
    }
    return false;
}

static bool sdmmc_send_cmd(uint32_t cmd_idx, uint32_t arg, bool expect_resp, bool check_crc,
                           bool send_init, uint32_t *out_resp)
{
    if (!sdmmc_wait_idle(100000u)) return false;

    SDMMC.rintsts.val = 0xFFFFFFFFu;
    SDMMC.cmdarg = arg;

    sdmmc_hw_cmd_t cmd;
    mem_zero(&cmd, sizeof cmd);
    cmd.cmd_index = cmd_idx;
    cmd.card_num = SDIO_SLOT;
    cmd.response_expect = expect_resp ? 1 : 0;
    cmd.check_response_crc = check_crc ? 1 : 0;
    cmd.send_init = send_init ? 1 : 0;
    cmd.wait_complete = 1;
    cmd.use_hold_reg = 1;
    cmd.start_command = 1;

    SDMMC.cmd = cmd;

    for (uint32_t t = 0; t < 200000u; t++) {
        uint32_t rint = SDMMC.rintsts.val;
        if (rint & SDMMC_LL_EVENT_HLE) {
            SDMMC.rintsts.val = 0xFFFFFFFFu;
            return false;
        }
        if (rint & SDMMC_LL_EVENT_CMD_DONE) {
            if ((rint & SDMMC_LL_EVENT_RTO) ||
                (check_crc && (rint & SDMMC_LL_EVENT_RCRC)) ||
                (rint & SDMMC_LL_EVENT_RESP_ERR)) {
                SDMMC.rintsts.val = 0xFFFFFFFFu;
                return false;
            }
            if (out_resp) *out_resp = SDMMC.resp[0];
            SDMMC.rintsts.val = 0xFFFFFFFFu;
            return true;
        }
        esp_rom_delay_us(1);
    }
    SDMMC.rintsts.val = 0xFFFFFFFFu;
    return false;
}

static bool sdio_cmd52(bool is_write, uint8_t fn, uint32_t addr, uint8_t in_val, uint8_t *out_val)
{
    uint32_t arg = (is_write ? (1u << 31) : 0u) |
                   ((uint32_t)(fn & 0x7u) << 28) |
                   ((is_write && out_val) ? (1u << 27) : 0u) |
                   ((addr & 0x1FFFFu) << 9) |
                   (uint32_t)(in_val & 0xFFu);
    uint32_t resp = 0;
    if (!sdmmc_send_cmd(52, arg, true, true, false, &resp)) return false;
    if ((resp >> 8) & 0xCBu) return false;
    if (out_val) *out_val = (uint8_t)(resp & 0xFFu);
    return true;
}

static bool sdio_cmd53(bool is_write, uint8_t fn, uint32_t addr, bool block_mode,
                       void *buf, size_t len)
{
    if (len == 0 || len > sizeof(s_dma_buf)) return false;
    if (!sdmmc_wait_idle(200000u)) return false;

    size_t block_size = block_mode ? SDIO_BLOCK_SIZE : len;
    size_t count      = block_mode ? (len / SDIO_BLOCK_SIZE) : (len == 512u ? 0u : len);
    size_t sync_len   = (len + 63u) & ~63u;
    if (sync_len > sizeof(s_dma_buf)) sync_len = sizeof(s_dma_buf);

    if (is_write) {
        mem_copy(s_dma_buf, buf, len);
    }
    /* Always flush s_dma_buf to RAM before DMA so stale dirty cache lines never overwrite RX data */
    esp_cache_msync(s_dma_buf, sync_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Fill DMA descriptor chain (up to 4096 bytes per descriptor) */
    mem_zero(s_dma_desc, sizeof(s_dma_desc));
    size_t rem = len;
    uint8_t *ptr = s_dma_buf;
    int d = 0;
    while (rem > 0) {
        size_t chunk = rem > 4096u ? 4096u : rem;
        s_dma_desc[d].second_address_chained = 1;
        s_dma_desc[d].owned_by_idmac = 1;
        if (d == 0) s_dma_desc[d].first_descriptor = 1;
        if (rem <= 4096u) {
            s_dma_desc[d].last_descriptor = 1;
        } else {
            s_dma_desc[d].disable_int_on_completion = 1;
        }
        s_dma_desc[d].buffer1_size = (chunk + 3u) & ~3u;
        s_dma_desc[d].buffer1_ptr = ptr;
        s_dma_desc[d].next_desc_ptr = (rem <= 4096u) ? NULL : &s_dma_desc[d + 1];
        ptr += chunk;
        rem -= chunk;
        d++;
    }
    esp_cache_msync(s_dma_desc, sizeof(s_dma_desc), ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Reset FIFO before starting DMA transfer */
    sdmmc_ll_reset_fifo(&SDMMC);
    for (int i = 0; i < 1000 && !sdmmc_ll_is_fifo_reset_done(&SDMMC); i++) {
        esp_rom_delay_us(1);
    }

    SDMMC.rintsts.val = 0xFFFFFFFFu;
    SDMMC.idsts.val   = 0xFFFFFFFFu;

    sdmmc_ll_set_data_transfer_len(&SDMMC, len);
    sdmmc_ll_set_block_size(&SDMMC, block_size);
    sdmmc_ll_set_desc_addr(&SDMMC, (uint32_t)(uintptr_t)&s_dma_desc[0]);
    sdmmc_ll_enable_dma(&SDMMC, true);
    sdmmc_ll_poll_demand(&SDMMC);

    uint32_t arg = (is_write ? (1u << 31) : 0u) |
                   ((uint32_t)(fn & 0x7u) << 28) |
                   (block_mode ? (1u << 27) : 0u) |
                   (1u << 26) | /* OP_CODE_INCREMENTING_ADDR */
                   ((addr & 0x1FFFFu) << 9) |
                   (uint32_t)(count & 0x1FFu);

    SDMMC.cmdarg = arg;

    sdmmc_hw_cmd_t cmd;
    mem_zero(&cmd, sizeof cmd);
    cmd.cmd_index = 53;
    cmd.card_num = SDIO_SLOT;
    cmd.response_expect = 1;
    cmd.check_response_crc = 1;
    cmd.data_expected = 1;
    cmd.rw = is_write ? 1 : 0;
    cmd.wait_complete = 1;
    cmd.use_hold_reg = 1;
    cmd.start_command = 1;

    SDMMC.cmd = cmd;

    uint32_t err_mask = SDMMC_LL_EVENT_RTO | SDMMC_LL_EVENT_DTO | SDMMC_LL_EVENT_HTO |
                        SDMMC_LL_EVENT_RCRC | SDMMC_LL_EVENT_DCRC |
                        SDMMC_LL_EVENT_RESP_ERR | SDMMC_LL_EVENT_HLE |
                        SDMMC_LL_EVENT_SBE | SDMMC_LL_EVENT_EBE;

    bool ok = false;
    for (uint32_t t = 0; t < 500000u; t++) {
        uint32_t rint = SDMMC.rintsts.val;
        if (rint & err_mask) {
            break;
        }
        if ((rint & SDMMC_LL_EVENT_DATA_OVER) && (rint & SDMMC_LL_EVENT_CMD_DONE)) {
            uint32_t resp = SDMMC.resp[0];
            if (!((resp >> 8) & 0xCBu)) {
                ok = true;
            }
            break;
        }
        esp_rom_delay_us(1);
    }

    sdmmc_ll_stop_dma(&SDMMC);
    SDMMC.rintsts.val = 0xFFFFFFFFu;
    SDMMC.idsts.val   = 0xFFFFFFFFu;
    sdmmc_wait_idle(50000u);

    if (ok && !is_write) {
        esp_cache_msync(s_dma_buf, sync_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        mem_copy(buf, s_dma_buf, len);
    }
    return ok;
}

static bool sdio_reg_read(uint32_t reg, void *dst, size_t len)
{
    reg = SDIO_REG(reg);
    if (len == 1) {
        return sdio_cmd52(false, 1, reg, 0, (uint8_t *)dst);
    }
    return sdio_cmd53(false, 1, reg, false, dst, len);
}

static bool sdio_reg_write(uint32_t reg, const void *src, size_t len)
{
    reg = SDIO_REG(reg);
    if (len == 1) {
        return sdio_cmd52(true, 1, reg, *(const uint8_t *)src, NULL);
    }
    return sdio_cmd53(true, 1, reg, false, (void *)src, len);
}

/* =========================================================================
 * SDIO Slave Reset & Initialization
 * ========================================================================= */

static void c6_hardware_reset(void)
{
    gpio_ll_func_sel(&GPIO, PIN_C6_RESET, PIN_FUNC_GPIO);
    gpio_ll_output_enable(&GPIO, PIN_C6_RESET);
    gpio_ll_set_level(&GPIO, PIN_C6_RESET, 0);
    task_sleep(50);
    gpio_ll_set_level(&GPIO, PIN_C6_RESET, 1);
    task_sleep(350); /* allow ESP32-C6 ROM + ESP-Hosted slave app to initialize SDIO slave */
}

static bool sdio_init_hardware(void)
{
    /* 1. Reset ESP32-C6 co-processor over GPIO 15 */
    c6_hardware_reset();

    /* 2. Enable SDMMC peripheral clock and reset controller */
    sdmmc_ll_enable_bus_clock(&SDMMC, true);
    sdmmc_ll_reset_register(&SDMMC);

    /* Enable peripheral clock source and initial divider BEFORE controller reset (CIU needs cclk_in) */
    esp_clk_tree_enable_src((soc_module_clk_t)SDMMC_CLK_SRC_PLL160M, true);
    sdmmc_ll_set_clock_div(&SDMMC, 2);
    sdmmc_ll_select_clk_source(&SDMMC, SDMMC_CLK_SRC_PLL160M);
    sdmmc_ll_init_phase_delay(&SDMMC);
    esp_rom_delay_us(20);

    /* 3. Reset controller, FIFO, DMA */
    sdmmc_ll_reset_controller(&SDMMC);
    sdmmc_ll_reset_fifo(&SDMMC);
    sdmmc_ll_reset_dma(&SDMMC);
    for (int i = 0; i < 2000; i++) {
        if (sdmmc_ll_is_controller_reset_done(&SDMMC) &&
            sdmmc_ll_is_fifo_reset_done(&SDMMC) &&
            sdmmc_ll_is_dma_reset_done(&SDMMC)) {
            break;
        }
        esp_rom_delay_us(1);
    }

    sdmmc_ll_clear_interrupt(&SDMMC, 0xFFFFFFFFu);
    sdmmc_ll_enable_interrupt(&SDMMC, 0xFFFFFFFFu, false);
    sdmmc_ll_enable_global_interrupt(&SDMMC, false);
    sdmmc_ll_enable_busy_clear_interrupt(&SDMMC, false);
    sdmmc_ll_init_dma(&SDMMC);

    sdmmc_ll_set_response_timeout(&SDMMC, 255);
    sdmmc_ll_set_data_timeout(&SDMMC, 0xFFFFFFu);
    sdmmc_ll_set_card_width(&SDMMC, SDIO_SLOT, SD_BUS_WIDTH_1_BIT);

    /* 4. Route GPIO matrix signals for Slot 1 */
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,  SD_CARD_INT_N_2_PAD_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, SD_CARD_DETECT_N_2_PAD_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,  SD_CARD_WRITE_PRT_2_PAD_IN_IDX, true);

    /* Drive D3 (GPIO 8) high during CMD0/CMD5 probing to select SDIO mode */
    gpio_ll_func_sel(&GPIO, PIN_SDIO_D3, PIN_FUNC_GPIO);
    gpio_ll_pullup_en(&GPIO, PIN_SDIO_D3);
    gpio_ll_set_level(&GPIO, PIN_SDIO_D3, 1);
    gpio_ll_output_enable(&GPIO, PIN_SDIO_D3);

    /* Drive CLK only (no input loopback) */
    gpio_ll_func_sel(&GPIO, PIN_SDIO_CLK, PIN_FUNC_GPIO);
    gpio_ll_input_disable(&GPIO, PIN_SDIO_CLK);
    gpio_ll_pullup_dis(&GPIO, PIN_SDIO_CLK);
    gpio_ll_pulldown_dis(&GPIO, PIN_SDIO_CLK);
    gpio_ll_set_drive_capability(&GPIO, PIN_SDIO_CLK, GPIO_DRIVE_CAP_3);
    esp_rom_gpio_connect_out_signal(PIN_SDIO_CLK, SD_CARD_CCLK_2_PAD_OUT_IDX, false, false);

    gpio_setup_sdio_pin(PIN_SDIO_CMD, SD_CARD_CCMD_2_PAD_IN_IDX,   SD_CARD_CCMD_2_PAD_OUT_IDX,   true);
    gpio_setup_sdio_pin(PIN_SDIO_D0,  SD_CARD_CDATA0_2_PAD_IN_IDX, SD_CARD_CDATA0_2_PAD_OUT_IDX, true);
    gpio_setup_sdio_pin(PIN_SDIO_D1,  SD_CARD_CDATA1_2_PAD_IN_IDX, SD_CARD_CDATA1_2_PAD_OUT_IDX, true);
    gpio_setup_sdio_pin(PIN_SDIO_D2,  SD_CARD_CDATA2_2_PAD_IN_IDX, SD_CARD_CDATA2_2_PAD_OUT_IDX, true);

    /* 5. Set 400 kHz probing clock */
    if (!sdmmc_set_clock_khz(400)) {
        kprintf("wifi: SDMMC 400kHz clock setup failed\n");
        return false;
    }
    task_sleep(10);

    /* 6. Send CMD0 (with 80-clock init sequence), then CMD5 (IO_SEND_OP_COND) */
    sdmmc_send_cmd(0, 0, false, false, true, NULL);
    esp_rom_delay_us(1000);

    uint32_t r4 = 0;
    bool cmd5_ok = false;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (sdmmc_send_cmd(5, 0, true, false, attempt == 0, &r4)) {
            cmd5_ok = true;
            break;
        }
        task_sleep(20);
    }
    if (!cmd5_ok) {
        kprintf("wifi: SDIO CMD5 probe failed (no response from ESP32-C6)\n");
        return false;
    }

    uint32_t ocr = (r4 & 0x00FF8000u) ? (r4 & 0x00FF8000u) : 0x00300000u;
    bool card_ready = false;
    for (int i = 0; i < 50; i++) {
        if (sdmmc_send_cmd(5, ocr, true, false, false, &r4) && (r4 & 0x80000000u)) {
            card_ready = true;
            break;
        }
        task_sleep(10);
    }
    if (!card_ready) {
        kprintf("wifi: SDIO CMD5 OCR negotiation timed out (r4=0x%08x)\n", r4);
        return false;
    }

    /* 7. CMD3 (SEND_RELATIVE_ADDR) and CMD7 (SELECT_CARD) */
    uint32_t r6 = 0;
    if (!sdmmc_send_cmd(3, 0, true, true, false, &r6)) {
        kprintf("wifi: SDIO CMD3 failed\n");
        return false;
    }
    uint16_t rca = (uint16_t)(r6 >> 16);
    uint32_t r1 = 0;
    if (!sdmmc_send_cmd(7, (uint32_t)rca << 16, true, true, false, &r1)) {
        kprintf("wifi: SDIO CMD7 select card failed\n");
        return false;
    }

    /* Connect D3 (GPIO 8) to SDMMC slot 1 data line 3 and enable 4-bit bus + high-speed */
    gpio_setup_sdio_pin(PIN_SDIO_D3, SD_CARD_CDATA3_2_PAD_IN_IDX, SD_CARD_CDATA3_2_PAD_OUT_IDX, true);

    uint8_t bus_w = 0;
    if (sdio_cmd52(false, 0, CCCR_BUS_WIDTH, 0, &bus_w)) {
        bus_w = (uint8_t)((bus_w & ~0x03u) | 0x02u); /* 4-bit bus */
        sdio_cmd52(true, 0, CCCR_BUS_WIDTH, bus_w, NULL);
        sdmmc_ll_set_card_width(&SDMMC, SDIO_SLOT, SD_BUS_WIDTH_4_BIT);
    }

    uint8_t hs = 0;
    if (sdio_cmd52(false, 0, CCCR_HIGHSPEED, 0, &hs)) {
        if (hs & 0x01u) {
            hs |= 0x02u; /* EHS */
            sdio_cmd52(true, 0, CCCR_HIGHSPEED, hs, NULL);
        }
    }

    /* Switch to 20 MHz high-speed clock */
    if (!sdmmc_set_clock_khz(20000)) {
        kprintf("wifi: SDMMC 20MHz clock switch failed\n");
        return false;
    }

    /* 8. Enable SDIO Function 1 and configure 512-byte block size */
    uint8_t fn_en = 0;
    sdio_cmd52(false, 0, CCCR_FN_ENABLE, 0, &fn_en);
    fn_en |= 0x02u;
    if (!sdio_cmd52(true, 0, CCCR_FN_ENABLE, fn_en, NULL)) {
        kprintf("wifi: failed to enable SDIO Function 1\n");
        return false;
    }

    bool fn1_ready = false;
    for (int i = 0; i < 50; i++) {
        uint8_t rdy = 0;
        if (sdio_cmd52(false, 0, CCCR_FN_READY, 0, &rdy) && (rdy & 0x02u)) {
            fn1_ready = true;
            break;
        }
        task_sleep(10);
    }
    if (!fn1_ready) {
        kprintf("wifi: SDIO Function 1 not ready\n");
        return false;
    }

    uint8_t int_en = 0x03u; /* IENM | IEN1 */
    sdio_cmd52(true, 0, CCCR_INT_ENABLE, int_en, NULL);

    /* Block size = 512 (0x0200) for Fn 0 and Fn 1 */
    sdio_cmd52(true, 0, CCCR_BLKSIZEL, 0x00u, NULL);
    sdio_cmd52(true, 0, CCCR_BLKSIZEH, 0x02u, NULL);
    sdio_cmd52(true, 0, FBR1_BLKSIZEL, 0x00u, NULL);
    sdio_cmd52(true, 0, FBR1_BLKSIZEH, 0x02u, NULL);

    /* Open ESP-Hosted data path on ESP32-C6 (Scratch Reg 7 = BIT(0)) */
    uint8_t open_dp = ESP_OPEN_DATAPATH;
    if (!sdio_reg_write(ESP_SLAVE_SCRATCH_REG_7, &open_dp, 1)) {
        kprintf("wifi: failed to write ESP_OPEN_DATAPATH\n");
        return false;
    }

    s_sdio_rx_bytes = 0;
    s_sdio_tx_bufs  = 0;
    s_seq_num       = 0;
    s_sdio_ready    = true;
    kprintf("wifi: SDIO Slot 1 ready (RCA=0x%04x, 4-bit @ 20 MHz)\n", rca);
    return true;
}

/* =========================================================================
 * ESP-Hosted Transport Framing (12-byte esp_payload_header)
 * ========================================================================= */

static uint16_t compute_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += buf[i];
    return (uint16_t)(sum & 0xFFFFu);
}

static bool sdio_send_packet(uint8_t if_type, uint8_t if_num, const uint8_t *payload, size_t payload_len)
{
    if (!s_sdio_ready || payload_len + 12u > sizeof(s_tx_packet_buf)) return false;

    size_t total_len = 12u + payload_len;
    size_t buf_needed = (total_len + 1536u - 1u) / 1536u;

    /* Poll slave token counter until enough 1536-byte buffers are free */
    bool has_buf = false;
    for (int poll = 0; poll < 200; poll++) {
        uint32_t token = 0;
        if (sdio_reg_read(ESP_SLAVE_TOKEN_RDATA, &token, 4)) {
            uint32_t wdata = (token >> 16) & 0xFFFu;
            uint32_t free_bufs = (wdata + 0x1000u - s_sdio_tx_bufs) & 0xFFFu;
            if (free_bufs >= buf_needed) {
                has_buf = true;
                break;
            }
        }
        esp_rom_delay_us(500);
    }
    if (!has_buf) return false;

    size_t block_len = ((total_len + SDIO_BLOCK_SIZE - 1u) / SDIO_BLOCK_SIZE) * SDIO_BLOCK_SIZE;
    mem_zero(s_tx_packet_buf, block_len);

    s_seq_num++;
    s_tx_packet_buf[0] = (uint8_t)((if_type & 0x0Fu) | ((if_num & 0x0Fu) << 4));
    s_tx_packet_buf[1] = 0;
    s_tx_packet_buf[2] = (uint8_t)(payload_len & 0xFFu);
    s_tx_packet_buf[3] = (uint8_t)((payload_len >> 8) & 0xFFu);
    s_tx_packet_buf[4] = 12u; /* sizeof(esp_payload_header) */
    s_tx_packet_buf[5] = 0u;
    s_tx_packet_buf[6] = 0u;  /* checksum lo */
    s_tx_packet_buf[7] = 0u;  /* checksum hi */
    s_tx_packet_buf[8] = (uint8_t)(s_seq_num & 0xFFu);
    s_tx_packet_buf[9] = (uint8_t)((s_seq_num >> 8) & 0xFFu);
    s_tx_packet_buf[10] = 0u;
    s_tx_packet_buf[11] = (if_type == ESP_PRIV_IF) ? 0x33u : 0u; /* ESP_PACKET_TYPE_EVENT */

    if (payload && payload_len > 0) {
        mem_copy(&s_tx_packet_buf[12], payload, payload_len);
    }

    uint16_t csum = compute_checksum(s_tx_packet_buf, total_len);
    s_tx_packet_buf[6] = (uint8_t)(csum & 0xFFu);
    s_tx_packet_buf[7] = (uint8_t)((csum >> 8) & 0xFFu);

    uint32_t addr = (ESP_SLAVE_CMD53_END - (uint32_t)total_len) & 0x1FFFFu;
    if (!sdio_cmd53(true, 1, addr, true, s_tx_packet_buf, block_len)) {
        return false;
    }

    s_sdio_tx_bufs = (s_sdio_tx_bufs + (uint32_t)buf_needed) & 0xFFFu;
    if (if_type == ESP_STA_IF) s_status.tx_packets++;
    return true;
}

/* Forward declarations for packet handlers */
static void handle_priv_packet(const uint8_t *payload, size_t len);
static void handle_serial_packet(const uint8_t *payload, size_t len);
static void handle_sta_rx_frame(const uint8_t *frame, size_t len);
static void dhcp_send_discover(void);

static void sdio_poll_rx_locked(void)
{
    static bool s_in_sdio_poll = false;
    if (!s_sdio_ready || s_in_sdio_poll) return;
    s_in_sdio_poll = true;

    for (int iter = 0; iter < 8; iter++) {
        uint8_t regs[20];
        mem_zero(regs, sizeof regs);
        if (!sdio_reg_read(ESP_SLAVE_INT_RAW_REG, regs, 20)) {
            s_in_sdio_poll = false;
            return;
        }

        uint32_t intr = (uint32_t)regs[0] | ((uint32_t)regs[1] << 8) |
                        ((uint32_t)regs[2] << 16) | ((uint32_t)regs[3] << 24);
        uint32_t pkt_reg = (uint32_t)regs[16] | ((uint32_t)regs[17] << 8) |
                           ((uint32_t)regs[18] << 16) | ((uint32_t)regs[19] << 24);

        if (intr != 0) {
            sdio_reg_write(ESP_SLAVE_INT_CLR_REG, &intr, 4);
        }

        uint32_t len_from_slave = ((pkt_reg & 0xFFFFFu) + 0x100000u - s_sdio_rx_bytes) % 0x100000u;
        if (len_from_slave == 0) break;

        if (len_from_slave > sizeof(s_rx_packet_buf) - SDIO_BLOCK_SIZE) {
            len_from_slave = sizeof(s_rx_packet_buf) - SDIO_BLOCK_SIZE;
        }
        size_t block_len = ((len_from_slave + SDIO_BLOCK_SIZE - 1u) / SDIO_BLOCK_SIZE) * SDIO_BLOCK_SIZE;
        uint32_t addr = (ESP_SLAVE_CMD53_END - len_from_slave) & 0x1FFFFu;

        if (!sdio_cmd53(false, 1, addr, true, s_rx_packet_buf, block_len)) {
            break;
        }
        s_sdio_rx_bytes = (s_sdio_rx_bytes + len_from_slave) % 0x100000u;

        /* De-frame one or more back-to-back esp_payload_header packets */
        size_t pos = 0;
        while (pos + 12u <= len_from_slave) {
            const uint8_t *hdr = &s_rx_packet_buf[pos];
            uint8_t  if_type = hdr[0] & 0x0Fu;
            uint16_t plen    = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
            uint16_t offset  = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);

            if (offset < 12u || plen == 0 || plen > 4096u || pos + offset + plen > block_len) {
                break;
            }

            const uint8_t *payload = hdr + offset;
            if (if_type == ESP_PRIV_IF) {
                handle_priv_packet(payload, plen);
            } else if (if_type == ESP_SERIAL_IF) {
                handle_serial_packet(payload, plen);
            } else if (if_type == ESP_STA_IF) {
                s_status.rx_packets++;
                handle_sta_rx_frame(payload, plen);
            }
            pos += (size_t)offset + (size_t)plen;
        }
    }
    s_in_sdio_poll = false;
}

/* =========================================================================
 * ESP_PRIV_IF Initial Capabilities Handshake
 * ========================================================================= */

static void handle_priv_packet(const uint8_t *payload, size_t len)
{
    if (len < 2) return;
    uint8_t evt_type = payload[0];
    if (evt_type == 0x22u) { /* ESP_PRIV_EVENT_INIT */
        /* Reply with Host capabilities (ESP32-C6 = 0x0D, TX/RX prio) */
        static const uint8_t cap_pkt[] = {
            0x22u, 15u,
            0x44u, 1u, 0u,      /* Cap_ext */
            0x45u, 1u, 0x0Du,   /* Target = ESP32-C6 */
            0x46u, 1u, 0u,      /* Host_cap */
            0x47u, 1u, 80u,     /* Task_tx_prio */
            0x48u, 1u, 60u      /* Task_rx_prio */
        };
        sdio_send_packet(ESP_PRIV_IF, 0, cap_pkt, sizeof cap_pkt);
        s_priv_init_done = true;
    }
}

/* =========================================================================
 * Minimal Protobuf Wire Format Codec for ESP-Hosted RPC
 * ========================================================================= */

static size_t pb_put_varint(uint8_t *dst, uint64_t v)
{
    size_t n = 0;
    while (v >= 0x80u) {
        dst[n++] = (uint8_t)((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    dst[n++] = (uint8_t)(v & 0x7Fu);
    return n;
}

static size_t pb_put_tag(uint8_t *dst, uint32_t field_no, uint8_t wire_type)
{
    return pb_put_varint(dst, ((uint64_t)field_no << 3) | (uint64_t)(wire_type & 0x7u));
}

static size_t pb_put_uint(uint8_t *dst, uint32_t field_no, uint64_t val)
{
    size_t n = pb_put_tag(dst, field_no, 0);
    n += pb_put_varint(dst + n, val);
    return n;
}

static size_t pb_put_int32(uint8_t *dst, uint32_t field_no, int32_t val)
{
    /* Protobuf int32 negative values are sign-extended to 64-bit varints */
    return pb_put_uint(dst, field_no, (uint64_t)(int64_t)val);
}

static size_t pb_put_bytes(uint8_t *dst, uint32_t field_no, const void *data, size_t len)
{
    size_t n = pb_put_tag(dst, field_no, 2);
    n += pb_put_varint(dst + n, len);
    if (len > 0 && data) {
        mem_copy(dst + n, data, len);
        n += len;
    }
    return n;
}

static bool pb_get_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t res = 0;
    int shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t b = buf[(*pos)++];
        res |= (uint64_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *out = res;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip_field(const uint8_t *buf, size_t len, size_t *pos, uint8_t wire_type)
{
    uint64_t v = 0;
    switch (wire_type) {
    case 0:
        return pb_get_varint(buf, len, pos, &v);
    case 1:
        if (*pos + 8u > len) return false;
        *pos += 8u;
        return true;
    case 2:
        if (!pb_get_varint(buf, len, pos, &v) || *pos + v > len) return false;
        *pos += (size_t)v;
        return true;
    case 5:
        if (*pos + 4u > len) return false;
        *pos += 4u;
        return true;
    default:
        return false;
    }
}

static int32_t pb_parse_resp_code(uint32_t msg_id, const uint8_t *buf, size_t len)
{
    uint32_t resp_field = (msg_id == RPC_RESP_WIFI_GET_MAC) ? 2u : 1u;
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!pb_get_varint(buf, len, &pos, &tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t  wtype = (uint8_t)(tag & 0x7u);
        if (field == resp_field && wtype == 0) {
            uint64_t v = 0;
            if (pb_get_varint(buf, len, &pos, &v)) return (int32_t)v;
            break;
        }
        if (!pb_skip_field(buf, len, &pos, wtype)) break;
    }
    return 0;
}

static void pb_parse_ap_record(const uint8_t *buf, size_t len, wifi_ap_t *ap)
{
    mem_zero(ap, sizeof(*ap));
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!pb_get_varint(buf, len, &pos, &tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t  wtype = (uint8_t)(tag & 0x7u);

        if (wtype == 2) {
            uint64_t blen = 0;
            if (!pb_get_varint(buf, len, &pos, &blen) || pos + blen > len) break;
            if (field == 1 && blen <= 6) {
                mem_copy(ap->bssid, buf + pos, (size_t)blen);
            } else if (field == 2) {
                size_t copy_n = (size_t)blen;
                if (copy_n > WIFI_SSID_MAX) copy_n = WIFI_SSID_MAX;
                mem_copy(ap->ssid, buf + pos, copy_n);
                ap->ssid[copy_n] = 0;
            }
            pos += (size_t)blen;
        } else if (wtype == 0) {
            uint64_t v = 0;
            if (!pb_get_varint(buf, len, &pos, &v)) break;
            if (field == 3) ap->channel  = (uint8_t)v;
            else if (field == 5) ap->rssi = (int8_t)(int32_t)v;
            else if (field == 6) ap->authmode = (uint8_t)v;
        } else {
            if (!pb_skip_field(buf, len, &pos, wtype)) break;
        }
    }
}

static void handle_rpc_event(uint32_t msg_id, const uint8_t *payload, size_t len)
{
    if (msg_id == RPC_EVT_STA_CONNECTED) {
        size_t pos = 0;
        while (pos < len) {
            uint64_t tag = 0;
            if (!pb_get_varint(payload, len, &pos, &tag)) break;
            uint32_t f1 = (uint32_t)(tag >> 3);
            uint8_t  w1 = (uint8_t)(tag & 0x7u);
            if (f1 == 2 && w1 == 2) {
                uint64_t slen = 0;
                if (!pb_get_varint(payload, len, &pos, &slen) || pos + slen > len) break;
                size_t spos = pos, send = pos + (size_t)slen;
                while (spos < send) {
                    uint64_t stag = 0;
                    if (!pb_get_varint(payload, send, &spos, &stag)) break;
                    uint32_t f2 = (uint32_t)(stag >> 3);
                    uint8_t  w2 = (uint8_t)(stag & 0x7u);
                    if (w2 == 2) {
                        uint64_t blen = 0;
                        if (!pb_get_varint(payload, send, &spos, &blen) || spos + blen > send) break;
                        if (f2 == 1) {
                            size_t n = (size_t)blen > WIFI_SSID_MAX ? WIFI_SSID_MAX : (size_t)blen;
                            mem_copy(s_status.ssid, payload + spos, n);
                            s_status.ssid[n] = 0;
                        } else if (f2 == 3 && blen == 6) {
                            mem_copy(s_status.bssid, payload + spos, 6);
                        }
                        spos += (size_t)blen;
                    } else if (w2 == 0) {
                        uint64_t v = 0;
                        if (!pb_get_varint(payload, send, &spos, &v)) break;
                        if (f2 == 4) s_status.channel = (uint8_t)v;
                        else if (f2 == 5) s_status.authmode = (uint8_t)v;
                    } else {
                        if (!pb_skip_field(payload, send, &spos, w2)) break;
                    }
                }
                pos = send;
            } else {
                if (!pb_skip_field(payload, len, &pos, w1)) break;
            }
        }
        s_status.state = WIFI_STATE_CONNECTED;
        s_status.disconnect_reason = 0;
        ksnprintf(s_status.last_msg, sizeof s_status.last_msg, "Associated with %s (ch %u), requesting IP...",
                  s_status.ssid, s_status.channel);
        kprintf("wifi: RPC_EVT_STA_CONNECTED ssid=\"%s\" bssid=%02X:%02X:%02X:%02X:%02X:%02X ch=%u auth=%u\n",
                s_status.ssid,
                s_status.bssid[0], s_status.bssid[1], s_status.bssid[2],
                s_status.bssid[3], s_status.bssid[4], s_status.bssid[5],
                (unsigned)s_status.channel, (unsigned)s_status.authmode);
        /* Kick off DHCP Discover over ESP_STA_IF */
        s_dhcp_state = 1;
        s_dhcp_last_tx_us = 0;
        dhcp_send_discover();
    } else if (msg_id == RPC_EVT_STA_DISCONNECTED) {
        uint32_t reason = 0;
        int32_t rssi = 0;
        uint8_t bssid[6] = {0};
        size_t pos = 0;
        while (pos < len) {
            uint64_t tag = 0;
            if (!pb_get_varint(payload, len, &pos, &tag)) break;
            uint32_t f1 = (uint32_t)(tag >> 3);
            uint8_t  w1 = (uint8_t)(tag & 0x7u);
            if (f1 == 2 && w1 == 2) {
                uint64_t slen = 0;
                if (!pb_get_varint(payload, len, &pos, &slen) || pos + slen > len) break;
                size_t spos = pos, send = pos + (size_t)slen;
                while (spos < send) {
                    uint64_t stag = 0;
                    if (!pb_get_varint(payload, send, &spos, &stag)) break;
                    uint32_t f2 = (uint32_t)(stag >> 3);
                    uint8_t  w2 = (uint8_t)(stag & 0x7u);
                    if (f2 == 3 && w2 == 2) {
                        uint64_t blen = 0;
                        if (!pb_get_varint(payload, send, &spos, &blen) || spos + blen > send) break;
                        if (blen == 6) mem_copy(bssid, payload + spos, 6);
                        spos += (size_t)blen;
                    } else if (f2 == 4 && w2 == 0) {
                        uint64_t v = 0;
                        if (pb_get_varint(payload, send, &spos, &v)) reason = (uint32_t)v;
                    } else if (f2 == 5 && w2 == 0) {
                        uint64_t v = 0;
                        if (pb_get_varint(payload, send, &spos, &v)) rssi = (int32_t)v;
                    } else {
                        if (!pb_skip_field(payload, send, &spos, w2)) break;
                    }
                }
                pos = send;
            } else {
                if (!pb_skip_field(payload, len, &pos, w1)) break;
            }
        }
        kprintf("wifi: RPC_EVT_STA_DISCONNECTED bssid=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d reason=%u\n",
                bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                (int)rssi, (unsigned)reason);
        s_status.disconnect_reason = (uint16_t)reason;
        s_status.state = WIFI_STATE_DISCONNECTED;
        mem_zero(s_status.ip, 4);
        mem_zero(s_status.netmask, 4);
        mem_zero(s_status.gateway, 4);
        s_dhcp_state = 0;
        ksnprintf(s_status.last_msg, sizeof s_status.last_msg, "Disconnected (reason %u)", (unsigned)reason);
    }
}

static void handle_serial_packet(const uint8_t *payload, size_t len)
{
    /* TLV format: [0x01, ep_len_lo, ep_len_hi, "RPCRsp"/"RPCEvt", 0x02, data_len_lo, data_len_hi, <pb_bytes>] */
    if (len < 12 || payload[0] != 0x01u) return;
    uint16_t ep_len = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
    if (3u + ep_len + 3u > len) return;
    size_t d_off = 3u + ep_len;
    if (payload[d_off] != 0x02u) return;
    uint16_t pb_len = (uint16_t)payload[d_off + 1] | ((uint16_t)payload[d_off + 2] << 8);
    const uint8_t *pb = payload + d_off + 3u;
    if (d_off + 3u + pb_len > len) pb_len = (uint16_t)(len - (d_off + 3u));

    uint32_t msg_type = 0, msg_id = 0, uid = 0;
    const uint8_t *sub_ptr = NULL;
    size_t sub_len = 0;

    size_t pos = 0;
    while (pos < pb_len) {
        uint64_t tag = 0;
        if (!pb_get_varint(pb, pb_len, &pos, &tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t  wtype = (uint8_t)(tag & 0x7u);

        if (wtype == 0) {
            uint64_t v = 0;
            if (!pb_get_varint(pb, pb_len, &pos, &v)) break;
            if (field == 1) msg_type = (uint32_t)v;
            else if (field == 2) msg_id = (uint32_t)v;
            else if (field == 3) uid = (uint32_t)v;
        } else if (wtype == 2) {
            uint64_t slen = 0;
            if (!pb_get_varint(pb, pb_len, &pos, &slen) || pos + slen > pb_len) break;
            if (field >= 512u) {
                sub_ptr = pb + pos;
                sub_len = (size_t)slen;
            }
            pos += (size_t)slen;
        } else {
            if (!pb_skip_field(pb, pb_len, &pos, wtype)) break;
        }
    }

    if (msg_type == RPC_TYPE_EVENT) {
        handle_rpc_event(msg_id, sub_ptr, sub_len);
    } else if (msg_type == RPC_TYPE_RESP) {
        if (s_wait_msg_id == msg_id && (s_wait_uid == 0 || s_wait_uid == uid)) {
            s_wait_resp_code = sub_ptr ? pb_parse_resp_code(msg_id, sub_ptr, sub_len) : 0;
            s_wait_payload_len = sub_len > sizeof(s_wait_payload) ? sizeof(s_wait_payload) : sub_len;
            if (sub_ptr && s_wait_payload_len > 0) {
                mem_copy(s_wait_payload, sub_ptr, s_wait_payload_len);
            }
            s_wait_done = true;
        }
    }
}

static bool rpc_call_locked(uint32_t req_id, uint32_t resp_id, const uint8_t *req_body, size_t req_body_len,
                            uint32_t timeout_ms, int32_t *out_resp_code)
{
    uint8_t pb_buf[512];
    size_t pb_len = 0;

    s_rpc_uid++;
    pb_len += pb_put_uint(pb_buf + pb_len, 1, RPC_TYPE_REQ);
    pb_len += pb_put_uint(pb_buf + pb_len, 2, req_id);
    pb_len += pb_put_uint(pb_buf + pb_len, 3, s_rpc_uid);
    pb_len += pb_put_bytes(pb_buf + pb_len, req_id, req_body, req_body_len);

    uint8_t tlv[540];
    tlv[0] = 0x01u;
    tlv[1] = 6u;
    tlv[2] = 0u;
    mem_copy(&tlv[3], "RPCRsp", 6);
    tlv[9]  = 0x02u;
    tlv[10] = (uint8_t)(pb_len & 0xFFu);
    tlv[11] = (uint8_t)((pb_len >> 8) & 0xFFu);
    mem_copy(&tlv[12], pb_buf, pb_len);

    s_wait_msg_id = resp_id;
    s_wait_uid    = s_rpc_uid;
    s_wait_done   = false;
    s_wait_resp_code = -1;
    s_wait_payload_len = 0;

    if (!sdio_send_packet(ESP_SERIAL_IF, 0, tlv, 12u + pb_len)) {
        s_wait_msg_id = 0;
        return false;
    }

    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;
    while (hal_time_now_us() < deadline) {
        sdio_poll_rx_locked();
        if (s_wait_done) {
            s_wait_msg_id = 0;
            if (out_resp_code) *out_resp_code = s_wait_resp_code;
            return true;
        }
        task_sleep(5);
    }
    s_wait_msg_id = 0;
    return false;
}

/* =========================================================================
 * Lightweight DHCPv4 + ARP + ICMP Echo Responder on ESP_STA_IF
 * ========================================================================= */

static uint16_t ip_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | (uint32_t)p[i + 1];
    }
    if (len & 1u) {
        sum += (uint32_t)p[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

static void dhcp_send_packet(uint8_t msg_type, const uint8_t *req_ip, const uint8_t *srv_ip)
{
    uint8_t pkt[360];
    mem_zero(pkt, sizeof pkt);

    /* 1. Ethernet header (14 bytes): broadcast dst */
    for (int i = 0; i < 6; i++) pkt[i] = 0xFFu;
    mem_copy(&pkt[6], s_status.mac, 6);
    pkt[12] = 0x08u; pkt[13] = 0x00u; /* IPv4 */

    /* 2. IPv4 header (20 bytes) + UDP header (8 bytes) + BOOTP (240 bytes) + options (32 bytes) */
    uint8_t *ip  = &pkt[14];
    uint8_t *udp = &pkt[34];
    uint8_t *bp  = &pkt[42];

    size_t opt_len = 0;
    uint8_t *opt = &bp[240];
    opt[opt_len++] = 53; opt[opt_len++] = 1; opt[opt_len++] = msg_type; /* DHCP Message Type */
    opt[opt_len++] = 61; opt[opt_len++] = 7; opt[opt_len++] = 1;        /* Client ID (MAC) */
    mem_copy(&opt[opt_len], s_status.mac, 6); opt_len += 6;
    opt[opt_len++] = 12; opt[opt_len++] = 7;                            /* Hostname: M5-Tab5 */
    mem_copy(&opt[opt_len], "M5-Tab5", 7); opt_len += 7;
    if (msg_type == 3 && req_ip) {
        opt[opt_len++] = 50; opt[opt_len++] = 4;                        /* Requested IP */
        mem_copy(&opt[opt_len], req_ip, 4); opt_len += 4;
    }
    if (msg_type == 3 && srv_ip && srv_ip[0]) {
        opt[opt_len++] = 54; opt[opt_len++] = 4;                        /* Server Identifier */
        mem_copy(&opt[opt_len], srv_ip, 4); opt_len += 4;
    }
    opt[opt_len++] = 55; opt[opt_len++] = 3;                            /* Parameter Request List */
    opt[opt_len++] = 1;  opt[opt_len++] = 3; opt[opt_len++] = 6;        /* Mask, Router, DNS */
    opt[opt_len++] = 255;                                               /* End */

    size_t bootp_len = 240u + opt_len;
    if (bootp_len < 300u) bootp_len = 300u;
    size_t udp_len   = 8u + bootp_len;
    size_t ip_len    = 20u + udp_len;

    /* Fill BOOTP */
    bp[0] = 1;  /* BOOTREQUEST */
    bp[1] = 1;  /* Ethernet */
    bp[2] = 6;  /* HLEN */
    bp[4] = (uint8_t)((s_dhcp_xid >> 24) & 0xFFu);
    bp[5] = (uint8_t)((s_dhcp_xid >> 16) & 0xFFu);
    bp[6] = (uint8_t)((s_dhcp_xid >> 8) & 0xFFu);
    bp[7] = (uint8_t)(s_dhcp_xid & 0xFFu);
    bp[10] = 0x80u; /* Broadcast flag */
    mem_copy(&bp[28], s_status.mac, 6);
    bp[236] = 99; bp[237] = 130; bp[238] = 83; bp[239] = 99; /* DHCP magic cookie */

    /* Fill UDP */
    udp[0] = 0; udp[1] = 68; /* src port 68 */
    udp[2] = 0; udp[3] = 67; /* dst port 67 */
    udp[4] = (uint8_t)((udp_len >> 8) & 0xFFu);
    udp[5] = (uint8_t)(udp_len & 0xFFu);
    udp[6] = 0; udp[7] = 0;  /* UDP checksum optional in IPv4 */

    /* Fill IPv4 */
    ip[0] = 0x45u;
    ip[2] = (uint8_t)((ip_len >> 8) & 0xFFu);
    ip[3] = (uint8_t)(ip_len & 0xFFu);
    ip[8] = 64u;  /* TTL */
    ip[9] = 17u;  /* UDP */
    for (int i = 16; i < 20; i++) ip[i] = 0xFFu; /* 255.255.255.255 */
    uint16_t csum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)((csum >> 8) & 0xFFu);
    ip[11] = (uint8_t)(csum & 0xFFu);

    s_dhcp_last_tx_us = hal_time_now_us();
    sdio_send_packet(ESP_STA_IF, 0, pkt, 14u + ip_len);
}

static void dhcp_send_discover(void)
{
    s_dhcp_xid += (uint32_t)(hal_time_now_us() & 0xFFFFu) + 1u;
    dhcp_send_packet(1, NULL, NULL);
}

static void arp_learn(const uint8_t ip[4], const uint8_t mac[6])
{
    if (!ip[0] || (mac[0] == 0xFFu && mac[1] == 0xFFu)) return;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_arp_cache[i].valid &&
            s_arp_cache[i].ip[0] == ip[0] && s_arp_cache[i].ip[1] == ip[1] &&
            s_arp_cache[i].ip[2] == ip[2] && s_arp_cache[i].ip[3] == ip[3]) {
            mem_copy(s_arp_cache[i].mac, mac, 6);
            return;
        }
    }
    uint8_t slot = s_arp_next_slot++ % ARP_CACHE_SIZE;
    s_arp_cache[slot].valid = true;
    mem_copy(s_arp_cache[slot].ip, ip, 4);
    mem_copy(s_arp_cache[slot].mac, mac, 6);
}

static bool arp_lookup(const uint8_t ip[4], uint8_t out_mac[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_arp_cache[i].valid &&
            s_arp_cache[i].ip[0] == ip[0] && s_arp_cache[i].ip[1] == ip[1] &&
            s_arp_cache[i].ip[2] == ip[2] && s_arp_cache[i].ip[3] == ip[3]) {
            mem_copy(out_mac, s_arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_send_request(const uint8_t target_ip[4])
{
    uint8_t pkt[42];
    mem_zero(pkt, sizeof pkt);
    for (int i = 0; i < 6; i++) pkt[i] = 0xFFu;
    mem_copy(&pkt[6], s_status.mac, 6);
    pkt[12] = 0x08u; pkt[13] = 0x06u;
    pkt[14] = 0x00u; pkt[15] = 0x01u; /* Ethernet */
    pkt[16] = 0x08u; pkt[17] = 0x00u; /* IPv4 */
    pkt[18] = 6u;    pkt[19] = 4u;
    pkt[20] = 0x00u; pkt[21] = 0x01u; /* ARP Request */
    mem_copy(&pkt[22], s_status.mac, 6);
    mem_copy(&pkt[28], s_status.ip, 4);
    mem_copy(&pkt[38], target_ip, 4);
    sdio_send_packet(ESP_STA_IF, 0, pkt, sizeof pkt);
}

static uint16_t tcp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             const uint8_t *tcp_seg, size_t tcp_len)
{
    uint32_t sum = 0;
    sum += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    sum += 6u; /* IPPROTO_TCP */
    sum += (uint32_t)tcp_len;
    for (size_t i = 0; i + 1 < tcp_len; i += 2) {
        sum += ((uint32_t)tcp_seg[i] << 8) | tcp_seg[i + 1];
    }
    if (tcp_len & 1u) {
        sum += (uint32_t)tcp_seg[tcp_len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

static bool tcp_send_segment_locked(tcp_conn_t *c, uint8_t flags, uint32_t seq, uint32_t ack,
                                    const void *data, size_t data_len)
{
    if (!c || data_len > 1360u) return false;
    bool via_tun = (is_tailscale_ip(c->remote_ip) && s_tun_tx_cb != NULL && s_tun_ip[0] != 0);
    const uint8_t *src_ip = via_tun ? s_tun_ip : s_status.ip;

    static uint8_t pkt[1420];
    size_t opt_len = (flags & TCP_SYN) ? 4u : 0u;
    size_t tcp_hdr_len = 20u + opt_len;
    size_t tcp_len = tcp_hdr_len + data_len;
    size_t ip_len  = 20u + tcp_len;

    mem_zero(pkt, 14u + 20u + tcp_hdr_len);
    mem_copy(&pkt[0], c->remote_mac, 6);
    mem_copy(&pkt[6], s_status.mac, 6);
    pkt[12] = 0x08u; pkt[13] = 0x00u;

    uint8_t *ip = &pkt[14];
    s_ip_id++;
    ip[0] = 0x45u;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)(ip_len & 0xFFu);
    ip[4] = (uint8_t)(s_ip_id >> 8);
    ip[5] = (uint8_t)(s_ip_id & 0xFFu);
    ip[6] = 0x40u; /* DF */
    ip[8] = 64u;   /* TTL */
    ip[9] = 6u;    /* TCP */
    mem_copy(&ip[12], src_ip, 4);
    mem_copy(&ip[16], c->remote_ip, 4);
    uint16_t hcsum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(hcsum >> 8);
    ip[11] = (uint8_t)(hcsum & 0xFFu);

    uint8_t *tcp = &pkt[34];
    tcp[0] = (uint8_t)(c->local_port >> 8);
    tcp[1] = (uint8_t)(c->local_port & 0xFFu);
    tcp[2] = (uint8_t)(c->remote_port >> 8);
    tcp[3] = (uint8_t)(c->remote_port & 0xFFu);
    tcp[4] = (uint8_t)(seq >> 24);
    tcp[5] = (uint8_t)(seq >> 16);
    tcp[6] = (uint8_t)(seq >> 8);
    tcp[7] = (uint8_t)(seq & 0xFFu);
    tcp[8] = (uint8_t)(ack >> 24);
    tcp[9] = (uint8_t)(ack >> 16);
    tcp[10] = (uint8_t)(ack >> 8);
    tcp[11] = (uint8_t)(ack & 0xFFu);
    tcp[12] = (uint8_t)((tcp_hdr_len / 4u) << 4);
    tcp[13] = flags;
    size_t free_win = (c->rx_count < TCP_RX_BUF_SIZE) ? (TCP_RX_BUF_SIZE - c->rx_count) : 0u;
    if (free_win > 32768u) free_win = 32768u;
    tcp[14] = (uint8_t)(free_win >> 8);
    tcp[15] = (uint8_t)(free_win & 0xFFu);

    if (opt_len == 4u) {
        uint16_t mss = via_tun ? 1280u : 1360u;
        tcp[20] = 2u; tcp[21] = 4u;
        tcp[22] = (uint8_t)(mss >> 8);
        tcp[23] = (uint8_t)(mss & 0xFFu);
    }
    if (data_len > 0 && data) {
        mem_copy(tcp + tcp_hdr_len, data, data_len);
    }

    uint16_t tcsum = tcp_checksum(src_ip, c->remote_ip, tcp, tcp_len);
    tcp[16] = (uint8_t)(tcsum >> 8);
    tcp[17] = (uint8_t)(tcsum & 0xFFu);

    if (via_tun) {
        static uint8_t s_tun_ip_pkt[1406];
        mem_copy(s_tun_ip_pkt, ip, ip_len);
        wifi_tun_tx_cb_t tx_cb = s_tun_tx_cb;
        mutex_unlock(&s_wifi_lock);
        bool ok = tx_cb(s_tun_ip_pkt, ip_len);
        mutex_lock(&s_wifi_lock);
        return ok;
    }
    return sdio_send_packet(ESP_STA_IF, 0, pkt, 14u + ip_len);
}

static size_t dns_skip_name(const uint8_t *msg, size_t len, size_t pos)
{
    while (pos < len) {
        uint8_t l = msg[pos];
        if (l == 0u) return pos + 1u;
        if ((l & 0xC0u) == 0xC0u) return pos + 2u;
        pos += 1u + (size_t)l;
    }
    return len + 1u;
}

static void handle_dns_rx(const uint8_t *msg, size_t len)
{
    if (len < 12u) return;
    uint16_t txid  = ((uint16_t)msg[0] << 8) | msg[1];
    uint16_t flags = ((uint16_t)msg[2] << 8) | msg[3];
    uint16_t qdcount = ((uint16_t)msg[4] << 8) | msg[5];
    uint16_t ancount = ((uint16_t)msg[6] << 8) | msg[7];
    if (txid != s_dns_txid || !(flags & 0x8000u)) return;

    size_t pos = 12u;
    for (uint16_t q = 0; q < qdcount; q++) {
        pos = dns_skip_name(msg, len, pos) + 4u;
        if (pos > len) return;
    }
    for (uint16_t a = 0; a < ancount; a++) {
        pos = dns_skip_name(msg, len, pos);
        if (pos + 10u > len) return;
        uint16_t rtype  = ((uint16_t)msg[pos] << 8) | msg[pos + 1];
        uint16_t rclass = ((uint16_t)msg[pos + 2] << 8) | msg[pos + 3];
        uint16_t rdlen  = ((uint16_t)msg[pos + 8] << 8) | msg[pos + 9];
        pos += 10u;
        if (pos + rdlen > len) return;
        if (rtype == 1u && rclass == 1u && rdlen == 4u) {
            mem_copy(s_dns_resolved_ip, &msg[pos], 4);
            s_dns_done = true;
            return;
        }
        pos += rdlen;
    }
}

static void handle_ipv4_rx_locked(const uint8_t *frame, size_t frame_len, const uint8_t *ip, size_t max_ip_len, bool from_tun)
{
    if (max_ip_len < 20u) return;
    size_t ihl = (size_t)(ip[0] & 0x0Fu) * 4u;
    size_t ip_total_len = ((size_t)ip[2] << 8) | ip[3];
    if (ihl < 20u || ihl > max_ip_len) return;
    if (ip_total_len > max_ip_len) ip_total_len = max_ip_len;
    if (ip_total_len < ihl) return;
    uint8_t proto = ip[9];

    /* Learn MAC of local-subnet sender on physical interface */
    if (!from_tun && frame && s_status.ip[0] != 0 &&
        ((ip[12] ^ s_status.ip[0]) & s_status.netmask[0]) == 0 &&
        ((ip[13] ^ s_status.ip[1]) & s_status.netmask[1]) == 0 &&
        ((ip[14] ^ s_status.ip[2]) & s_status.netmask[2]) == 0 &&
        ((ip[15] ^ s_status.ip[3]) & s_status.netmask[3]) == 0) {
        arp_learn(&ip[12], &frame[6]);
    }

    /* 2. ICMP Echo Request (ping) Responder */
    if (proto == 1u && ip_total_len >= ihl + 8u && ip_total_len <= 1480u) {
        const uint8_t *icmp = ip + ihl;
        if (icmp[0] == 8u && icmp[1] == 0u) {
            bool for_sta = (!from_tun && s_status.ip[0] != 0 &&
                            ip[16] == s_status.ip[0] && ip[17] == s_status.ip[1] &&
                            ip[18] == s_status.ip[2] && ip[19] == s_status.ip[3]);
            bool for_tun = (from_tun && s_tun_ip[0] != 0 &&
                            ip[16] == s_tun_ip[0] && ip[17] == s_tun_ip[1] &&
                            ip[18] == s_tun_ip[2] && ip[19] == s_tun_ip[3]);
            if (for_sta && frame && frame_len >= 14u + ip_total_len) {
                static uint8_t reply[1514];
                size_t rlen = 14u + ip_total_len;
                mem_copy(reply, frame, rlen);
                mem_copy(&reply[0], &frame[6], 6);
                mem_copy(&reply[6], s_status.mac, 6);
                mem_copy(&reply[14 + 12], s_status.ip, 4);
                mem_copy(&reply[14 + 16], &ip[12], 4);
                reply[14 + 10] = 0; reply[14 + 11] = 0;
                uint16_t hcsum = ip_checksum(&reply[14], ihl);
                reply[14 + 10] = (uint8_t)(hcsum >> 8);
                reply[14 + 11] = (uint8_t)(hcsum & 0xFFu);

                uint8_t *ricmp = &reply[14 + ihl];
                size_t icmp_len = ip_total_len - ihl;
                ricmp[0] = 0; /* Echo Reply */
                ricmp[2] = 0; ricmp[3] = 0;
                uint16_t icsum = ip_checksum(ricmp, icmp_len);
                ricmp[2] = (uint8_t)(icsum >> 8);
                ricmp[3] = (uint8_t)(icsum & 0xFFu);
                sdio_send_packet(ESP_STA_IF, 0, reply, rlen);
            } else if (for_tun && s_tun_tx_cb) {
                static uint8_t rip[1480];
                mem_copy(rip, ip, ip_total_len);
                mem_copy(&rip[12], s_tun_ip, 4);
                mem_copy(&rip[16], &ip[12], 4);
                rip[10] = 0; rip[11] = 0;
                uint16_t hcsum = ip_checksum(rip, ihl);
                rip[10] = (uint8_t)(hcsum >> 8);
                rip[11] = (uint8_t)(hcsum & 0xFFu);

                uint8_t *ricmp = rip + ihl;
                size_t icmp_len = ip_total_len - ihl;
                ricmp[0] = 0; /* Echo Reply */
                ricmp[2] = 0; ricmp[3] = 0;
                uint16_t icsum = ip_checksum(ricmp, icmp_len);
                ricmp[2] = (uint8_t)(icsum >> 8);
                ricmp[3] = (uint8_t)(icsum & 0xFFu);
                wifi_tun_tx_cb_t tx_cb = s_tun_tx_cb;
                mutex_unlock(&s_wifi_lock);
                tx_cb(rip, ip_total_len);
                mutex_lock(&s_wifi_lock);
            }
        }
        return;
    }

    /* 3. UDP: DNS Client (src port 53), custom UDP RX callback (e.g. WireGuard :41641), & DHCPv4 Client */
    if (proto == 17u && ip_total_len >= ihl + 8u) {
        const uint8_t *udp = ip + ihl;
        uint16_t src_port = ((uint16_t)udp[0] << 8) | (uint16_t)udp[1];
        uint16_t dst_port = ((uint16_t)udp[2] << 8) | (uint16_t)udp[3];
        uint16_t udp_hdr_len = ((uint16_t)udp[4] << 8) | (uint16_t)udp[5];
        size_t udp_payload_len = ip_total_len - ihl - 8u;
        if (udp_hdr_len >= 8u && (size_t)(udp_hdr_len - 8u) < udp_payload_len) {
            udp_payload_len = (size_t)(udp_hdr_len - 8u);
        }

        if ((src_port == 53u || src_port == 5353u) && udp_payload_len >= 12u) {
            handle_dns_rx(udp + 8u, udp_payload_len);
            return;
        }
        if (s_udp_rx_cb && dst_port == s_udp_rx_port) {
            uint8_t src_ip_copy[4];
            mem_copy(src_ip_copy, &ip[12], 4);
            /* Copy payload so callback can run without holding s_wifi_lock */
            static uint8_t s_udp_cb_buf[1600];
            size_t copy_len = udp_payload_len > sizeof(s_udp_cb_buf) ? sizeof(s_udp_cb_buf) : udp_payload_len;
            mem_copy(s_udp_cb_buf, udp + 8u, copy_len);
            wifi_udp_rx_cb_t cb = s_udp_rx_cb;
            mutex_unlock(&s_wifi_lock);
            cb(src_ip_copy, src_port, dst_port, s_udp_cb_buf, copy_len);
            mutex_lock(&s_wifi_lock);
            return;
        }
        if (src_port != 67u || dst_port != 68u || udp_payload_len < 240u) return;

        const uint8_t *bp = udp + 8u;
        size_t bp_len = udp_payload_len;
        uint32_t xid = ((uint32_t)bp[4] << 24) | ((uint32_t)bp[5] << 16) |
                       ((uint32_t)bp[6] << 8) | (uint32_t)bp[7];
        if (bp[0] != 2u || xid != s_dhcp_xid) return;
        if (bp[236] != 99u || bp[237] != 130u || bp[238] != 83u || bp[239] != 99u) return;

        uint8_t msg_type = 0;
        uint8_t mask[4] = {255, 255, 255, 0};
        uint8_t gw[4]   = {0, 0, 0, 0};
        uint8_t srv[4]  = {0, 0, 0, 0};
        uint8_t dns[4]  = {0, 0, 0, 0};

        size_t p = 240u;
        while (p < bp_len) {
            uint8_t code = bp[p++];
            if (code == 0u) continue;
            if (code == 255u) break;
            if (p >= bp_len) break;
            uint8_t olen = bp[p++];
            if (p + olen > bp_len) break;
            if (code == 53u && olen >= 1u) msg_type = bp[p];
            else if (code == 1u && olen == 4u) mem_copy(mask, &bp[p], 4);
            else if (code == 3u && olen >= 4u) mem_copy(gw, &bp[p], 4);
            else if (code == 6u && olen >= 4u) mem_copy(dns, &bp[p], 4);
            else if (code == 54u && olen == 4u) mem_copy(srv, &bp[p], 4);
            p += olen;
        }

        if (msg_type == 2u) { /* DHCP Offer */
            mem_copy(s_dhcp_offered_ip, &bp[16], 4);
            mem_copy(s_dhcp_server_ip, srv, 4);
            s_dhcp_state = 2;
            dhcp_send_packet(3, s_dhcp_offered_ip, s_dhcp_server_ip);
        } else if (msg_type == 5u) { /* DHCP Ack */
            mem_copy(s_status.ip, &bp[16], 4);
            mem_copy(s_status.netmask, mask, 4);
            mem_copy(s_status.gateway, gw, 4);
            if (dns[0]) mem_copy(s_dns_ip, dns, 4);
            else if (gw[0]) mem_copy(s_dns_ip, gw, 4);
            if (!from_tun && frame && ip[12] != 0) {
                arp_learn(&ip[12], &frame[6]);
            }
            s_dhcp_state = 3;
            ksnprintf(s_status.last_msg, sizeof s_status.last_msg,
                      "Connected to %s (IP %u.%u.%u.%u)",
                      s_status.ssid,
                      s_status.ip[0], s_status.ip[1], s_status.ip[2], s_status.ip[3]);
        }
        return;
    }

    /* 4. TCP Client State Machine (IPPROTO_TCP = 6) */
    if (proto == 6u && ip_total_len >= ihl + 20u) {
        const uint8_t *tcp = ip + ihl;
        uint16_t src_port = ((uint16_t)tcp[0] << 8) | tcp[1];
        uint16_t dst_port = ((uint16_t)tcp[2] << 8) | tcp[3];

        tcp_conn_t *c = NULL;
        for (int s = 0; s < WIFI_MAX_TCP_SOCKETS; s++) {
            tcp_conn_t *cand = &s_tcp_conns[s];
            if (cand->state != TCP_STATE_CLOSED &&
                cand->local_port == dst_port &&
                cand->remote_port == src_port &&
                ip[12] == cand->remote_ip[0] && ip[13] == cand->remote_ip[1] &&
                ip[14] == cand->remote_ip[2] && ip[15] == cand->remote_ip[3]) {
                c = cand;
                break;
            }
        }
        if (!c) return;

        uint32_t seq = ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) |
                       ((uint32_t)tcp[6] << 8)  | tcp[7];
        uint32_t ack = ((uint32_t)tcp[8] << 24) | ((uint32_t)tcp[9] << 16) |
                       ((uint32_t)tcp[10] << 8) | tcp[11];
        size_t doff = (size_t)(tcp[12] >> 4) * 4u;
        uint8_t flags = tcp[13];
        if (doff < 20u || ihl + doff > ip_total_len) return;
        const uint8_t *payload = tcp + doff;
        size_t plen = ip_total_len - ihl - doff;

        if (flags & TCP_RST) {
            c->state = TCP_STATE_CLOSED;
            return;
        }

        if (c->state == TCP_STATE_SYN_SENT) {
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->snd_nxt) {
                c->snd_una = ack;
                c->rcv_nxt = seq + 1u;
                c->state   = TCP_STATE_ESTABLISHED;
                tcp_send_segment_locked(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
            }
            return;
        }

        if (flags & TCP_ACK) {
            int32_t diff = (int32_t)(ack - c->snd_una);
            if (diff > 0) c->snd_una = ack;
        }

        bool need_ack = (flags & TCP_SYN) != 0;
        if (plen > 0 && c->rx_buf) {
            int32_t offset = (int32_t)(seq - c->rcv_nxt);
            if (offset <= 0 && (size_t)(-offset) < plen) {
                size_t skip = (size_t)(-offset);
                const uint8_t *new_data = payload + skip;
                size_t new_len = plen - skip;
                size_t space = TCP_RX_BUF_SIZE - c->rx_count;
                if (new_len > space) new_len = space;
                for (size_t i = 0; i < new_len; i++) {
                    c->rx_buf[c->rx_head] = new_data[i];
                    c->rx_head = (c->rx_head + 1u) % TCP_RX_BUF_SIZE;
                }
                c->rx_count += new_len;
                c->rcv_nxt  += (uint32_t)new_len;
            }
            need_ack = true;
        }

        if (flags & TCP_FIN) {
            if (seq + (uint32_t)plen == c->rcv_nxt) {
                c->rcv_nxt++;
                c->state = TCP_STATE_CLOSE_WAIT;
            }
            need_ack = true;
        }

        if (need_ack) {
            tcp_send_segment_locked(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
        }
    }
}

static void handle_sta_rx_frame(const uint8_t *frame, size_t len)
{
    if (len < 14) return;
    uint16_t ethertype = ((uint16_t)frame[12] << 8) | (uint16_t)frame[13];

    /* 1. ARP Request Responder & ARP Reply Cache (0x0806) */
    if (ethertype == 0x0806u && len >= 42) {
        uint16_t oper = ((uint16_t)frame[20] << 8) | (uint16_t)frame[21];
        arp_learn(&frame[28], &frame[22]);
        if (oper == 1 && s_status.ip[0] != 0 &&
            frame[38] == s_status.ip[0] && frame[39] == s_status.ip[1] &&
            frame[40] == s_status.ip[2] && frame[41] == s_status.ip[3]) {
            uint8_t reply[42];
            mem_copy(&reply[0], &frame[6], 6);
            mem_copy(&reply[6], s_status.mac, 6);
            reply[12] = 0x08u; reply[13] = 0x06u;
            mem_copy(&reply[14], &frame[14], 6);
            reply[20] = 0x00u; reply[21] = 0x02u; /* ARP Reply */
            mem_copy(&reply[22], s_status.mac, 6);
            mem_copy(&reply[28], s_status.ip, 4);
            mem_copy(&reply[32], &frame[22], 10);
            sdio_send_packet(ESP_STA_IF, 0, reply, sizeof reply);
        }
        return;
    }

    if (ethertype != 0x0800u || len < 34) return;
    handle_ipv4_rx_locked(frame, len, &frame[14], len - 14u, false);
}


/* =========================================================================
 * Saved Credentials (/home/wifi.cfg + ESP32-C6 NVS Flash)
 * ========================================================================= */

static bool load_cfg_from_fs(char *ssid_out, char *pass_out, bool *auto_out)
{
    int fd = vfs_open(WIFI_CFG_PATH, VFS_O_READ);
    if (fd < 0) return false;

    char buf[256];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) return false;
    buf[n] = 0;

    char ssid[WIFI_SSID_MAX + 1] = "";
    char pass[WIFI_PASS_MAX + 1] = "";
    bool auto_c = true;

    int pos = 0;
    while (pos < n) {
        char line[160];
        int l = 0;
        while (pos < n && buf[pos] != '\n' && buf[pos] != '\r') {
            if (l < (int)sizeof(line) - 1) line[l++] = buf[pos];
            pos++;
        }
        while (pos < n && (buf[pos] == '\n' || buf[pos] == '\r')) pos++;
        line[l] = 0;

        if (line[0] == 's' && line[1] == 's' && line[2] == 'i' && line[3] == 'd' && line[4] == '=') {
            str_copy(ssid, sizeof ssid, line + 5);
        } else if (line[0] == 'p' && line[1] == 'a' && line[2] == 's' && line[3] == 's' && line[4] == '=') {
            str_copy(pass, sizeof pass, line + 5);
        } else if (line[0] == 'a' && line[1] == 'u' && line[2] == 't' && line[3] == 'o' && line[4] == '=') {
            auto_c = (line[5] != '0');
        }
    }

    if (!ssid[0]) return false;
    if (ssid_out) str_copy(ssid_out, WIFI_SSID_MAX + 1, ssid);
    if (pass_out) str_copy(pass_out, WIFI_PASS_MAX + 1, pass);
    if (auto_out) *auto_out = auto_c;
    return true;
}

static bool save_cfg_to_fs(const char *ssid, const char *pass, bool auto_c)
{
    vfs_mkdir("/home");
    int fd = vfs_open(WIFI_CFG_PATH, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return false;

    char buf[256];
    int len = ksnprintf(buf, sizeof buf, "ssid=%s\npass=%s\nauto=%d\n",
                        ssid ? ssid : "", pass ? pass : "", auto_c ? 1 : 0);
    int w = vfs_write(fd, buf, (size_t)len);
    vfs_close(fd);
    return w == len;
}

/* Read saved STA config stored in ESP32-C6 NVS flash (survives power-cycle even on RAMFS) */
static bool load_cfg_from_c6_nvs_locked(char *ssid_out, char *pass_out)
{
    uint8_t req[8];
    size_t req_len = pb_put_uint(req, 1, 0); /* WIFI_IF_STA = 0 */
    int32_t resp_code = -1;
    if (!rpc_call_locked(RPC_REQ_WIFI_GET_CONFIG, RPC_RESP_WIFI_GET_CONFIG, req, req_len, 1500, &resp_code) ||
        resp_code != 0) {
        return false;
    }

    /* Parse Resp_WifiGetConfig: field 3 (cfg) -> field 2 (sta) -> field 1 (ssid), field 2 (password) */
    const uint8_t *buf = s_wait_payload;
    size_t len = s_wait_payload_len;
    size_t pos = 0;
    char ssid[WIFI_SSID_MAX + 1] = "";
    char pass[WIFI_PASS_MAX + 1] = "";

    while (pos < len) {
        uint64_t tag = 0;
        if (!pb_get_varint(buf, len, &pos, &tag)) break;
        uint32_t f1 = (uint32_t)(tag >> 3);
        uint8_t  w1 = (uint8_t)(tag & 0x7u);
        if (f1 == 3 && w1 == 2) {
            uint64_t cfg_len = 0;
            if (!pb_get_varint(buf, len, &pos, &cfg_len) || pos + cfg_len > len) break;
            size_t cpos = pos, cend = pos + (size_t)cfg_len;
            while (cpos < cend) {
                uint64_t ctag = 0;
                if (!pb_get_varint(buf, cend, &cpos, &ctag)) break;
                uint32_t f2 = (uint32_t)(ctag >> 3);
                uint8_t  w2 = (uint8_t)(ctag & 0x7u);
                if (f2 == 2 && w2 == 2) {
                    uint64_t sta_len = 0;
                    if (!pb_get_varint(buf, cend, &cpos, &sta_len) || cpos + sta_len > cend) break;
                    size_t spos = cpos, send = cpos + (size_t)sta_len;
                    while (spos < send) {
                        uint64_t stag = 0;
                        if (!pb_get_varint(buf, send, &spos, &stag)) break;
                        uint32_t f3 = (uint32_t)(stag >> 3);
                        uint8_t  w3 = (uint8_t)(stag & 0x7u);
                        if (w3 == 2) {
                            uint64_t blen = 0;
                            if (!pb_get_varint(buf, send, &spos, &blen) || spos + blen > send) break;
                            if (f3 == 1) {
                                size_t n = (size_t)blen > WIFI_SSID_MAX ? WIFI_SSID_MAX : (size_t)blen;
                                mem_copy(ssid, buf + spos, n);
                                ssid[n] = 0;
                            } else if (f3 == 2) {
                                size_t n = (size_t)blen > WIFI_PASS_MAX ? WIFI_PASS_MAX : (size_t)blen;
                                mem_copy(pass, buf + spos, n);
                                pass[n] = 0;
                            }
                            spos += (size_t)blen;
                        } else {
                            if (!pb_skip_field(buf, send, &spos, w3)) break;
                        }
                    }
                    cpos = send;
                } else {
                    if (!pb_skip_field(buf, cend, &cpos, w2)) break;
                }
            }
            pos = cend;
        } else {
            if (!pb_skip_field(buf, len, &pos, w1)) break;
        }
    }

    if (!ssid[0] || str_eq(ssid, "M5Stack-Production")) return false;
    if (ssid_out) str_copy(ssid_out, WIFI_SSID_MAX + 1, ssid);
    if (pass_out) str_copy(pass_out, WIFI_PASS_MAX + 1, pass);
    return true;
}

static bool set_c6_sta_config_locked(const char *ssid, const char *password)
{
    /*
     * Important ESP-Hosted slave quirk (slave_control.c lines 864-869):
     * req_wifi_set_config() dereferences p_c_sta->threshold (field 9) and
     * p_c_sta->pmf_cfg (field 10) without NULL checks! Always encode both submessages.
     */
    uint8_t thresh[16];
    size_t thresh_len = 0;
    thresh_len += pb_put_int32(thresh + thresh_len, 1, -127); /* rssi = -127 */
    thresh_len += pb_put_uint(thresh + thresh_len, 2, 0);     /* authmode = WIFI_AUTH_OPEN */

    uint8_t pmf[16];
    size_t pmf_len = 0;
    pmf_len += pb_put_uint(pmf + pmf_len, 1, 1); /* capable = true */
    pmf_len += pb_put_uint(pmf + pmf_len, 2, 0); /* required = false */

    uint8_t sta[200];
    size_t sta_len = 0;
    sta_len += pb_put_bytes(sta + sta_len, 1, ssid ? ssid : "", str_len(ssid));
    sta_len += pb_put_bytes(sta + sta_len, 2, password ? password : "", str_len(password));
    sta_len += pb_put_uint(sta + sta_len, 3, 0); /* scan_method (slave sets WIFI_ALL_CHANNEL_SCAN) */
    sta_len += pb_put_uint(sta + sta_len, 4, 0); /* bssid_set = false */
    sta_len += pb_put_uint(sta + sta_len, 6, 0); /* channel = 0 */
    sta_len += pb_put_uint(sta + sta_len, 7, 0); /* listen_interval = 0 */
    sta_len += pb_put_uint(sta + sta_len, 8, 0); /* sort_method = WIFI_CONNECT_AP_BY_SIGNAL */
    sta_len += pb_put_bytes(sta + sta_len, 9, thresh, thresh_len);
    sta_len += pb_put_bytes(sta + sta_len, 10, pmf, pmf_len);
    sta_len += pb_put_uint(sta + sta_len, 14, 0x1Eu); /* he_dcm_max_constellation_tx=3, rx=3 */

    uint8_t cfg[220];
    size_t cfg_len = pb_put_bytes(cfg, 2, sta, sta_len); /* wifi_config.sta = field 2 */

    uint8_t req[240];
    size_t req_len = 0;
    req_len += pb_put_uint(req + req_len, 1, 0); /* iface = WIFI_IF_STA (0) */
    req_len += pb_put_bytes(req + req_len, 2, cfg, cfg_len);

    int32_t resp_code = -1;
    return rpc_call_locked(RPC_REQ_WIFI_SET_CONFIG, RPC_RESP_WIFI_SET_CONFIG,
                           req, req_len, 2000, &resp_code) && resp_code == 0;
}

/* =========================================================================
 * Public Wi-Fi Driver API
 * ========================================================================= */

static void ensure_lock(void)
{
    if (!s_lock_ready) {
        mutex_init(&s_wifi_lock);
        s_lock_ready = true;
    }
}

bool wifi_init(void)
{
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    if (s_wifi_inited) {
        mutex_unlock(&s_wifi_lock);
        return true;
    }

    set_msg("Initializing ESP32-C6 SDIO link...");
    if (!sdio_init_hardware()) {
        s_status.state = WIFI_STATE_ERROR;
        set_msg("ESP32-C6 SDIO init failed");
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    /* Wait up to 1.5s for ESP_PRIV_EVENT_INIT from slave and send capability response */
    s_priv_init_done = false;
    uint64_t t0 = hal_time_now_us();
    while (hal_time_now_us() - t0 < 1500000ULL) {
        sdio_poll_rx_locked();
        if (s_priv_init_done) break;
        task_sleep(10);
    }

    /* Build WifiInit default config with nvs_enable = 1 and magic = 0x1F2F3F4F */
    uint8_t icfg[96];
    size_t ilen = 0;
    ilen += pb_put_uint(icfg + ilen, 1, 10);          /* static_rx_buf_num */
    ilen += pb_put_uint(icfg + ilen, 2, 32);          /* dynamic_rx_buf_num */
    ilen += pb_put_uint(icfg + ilen, 3, 1);           /* tx_buf_type */
    ilen += pb_put_uint(icfg + ilen, 4, 0);           /* static_tx_buf_num */
    ilen += pb_put_uint(icfg + ilen, 5, 32);          /* dynamic_tx_buf_num */
    ilen += pb_put_uint(icfg + ilen, 6, 0);           /* cache_tx_buf_num */
    ilen += pb_put_uint(icfg + ilen, 7, 0);           /* csi_enable */
    ilen += pb_put_uint(icfg + ilen, 8, 1);           /* ampdu_rx_enable */
    ilen += pb_put_uint(icfg + ilen, 9, 1);           /* ampdu_tx_enable */
    ilen += pb_put_uint(icfg + ilen, 10, 0);          /* amsdu_tx_enable */
    ilen += pb_put_uint(icfg + ilen, 11, 1);          /* nvs_enable = 1 */
    ilen += pb_put_uint(icfg + ilen, 12, 0);          /* nano_enable */
    ilen += pb_put_uint(icfg + ilen, 13, 6);          /* rx_ba_win */
    ilen += pb_put_uint(icfg + ilen, 14, 0);          /* wifi_task_core_id */
    ilen += pb_put_uint(icfg + ilen, 15, 752);        /* beacon_max_len */
    ilen += pb_put_uint(icfg + ilen, 16, 32);         /* mgmt_sbuf_num */
    ilen += pb_put_uint(icfg + ilen, 17, 1);          /* feature_caps (WPA3) */
    ilen += pb_put_uint(icfg + ilen, 18, 0);          /* sta_disconnected_pm */
    ilen += pb_put_uint(icfg + ilen, 19, 7);          /* espnow_max_encrypt_num */
    ilen += pb_put_uint(icfg + ilen, 20, 0x1F2F3F4Fu);/* WIFI_INIT_CONFIG_MAGIC */

    uint8_t req_init[112];
    size_t req_init_len = pb_put_bytes(req_init, 1, icfg, ilen);
    int32_t resp_code = -1;
    if (!rpc_call_locked(RPC_REQ_WIFI_INIT, RPC_RESP_WIFI_INIT, req_init, req_init_len, 3000, &resp_code) ||
        resp_code != 0) {
        s_status.state = WIFI_STATE_ERROR;
        set_msg("ESP-Hosted RPC WifiInit failed");
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    /* Persist Wi-Fi credentials in ESP32-C6 NVS flash (WIFI_STORAGE_FLASH = 0) */
    uint8_t req_storage[8];
    size_t req_storage_len = pb_put_uint(req_storage, 1, 0);
    rpc_call_locked(RPC_REQ_WIFI_SET_STORAGE, RPC_RESP_WIFI_SET_STORAGE, req_storage, req_storage_len, 1500, NULL);

    /* Set Station mode (WIFI_MODE_STA = 1) and start Wi-Fi */
    uint8_t req_mode[8];
    size_t req_mode_len = pb_put_uint(req_mode, 1, 1);
    rpc_call_locked(RPC_REQ_WIFI_SET_MODE, RPC_RESP_WIFI_SET_MODE, req_mode, req_mode_len, 2000, NULL);
    rpc_call_locked(RPC_REQ_WIFI_START, RPC_RESP_WIFI_START, NULL, 0, 3000, NULL);

    /* Disable Wi-Fi power save for responsive DHCP and ping */
    uint8_t req_ps[8];
    size_t req_ps_len = pb_put_uint(req_ps, 1, 0); /* WIFI_PS_NONE = 0 */
    rpc_call_locked(RPC_REQ_WIFI_SET_PS, RPC_RESP_WIFI_SET_PS, req_ps, req_ps_len, 1500, NULL);

    /* Read station MAC address */
    uint8_t req_mac[8];
    size_t req_mac_len = pb_put_uint(req_mac, 1, 0);
    if (rpc_call_locked(RPC_REQ_WIFI_GET_MAC, RPC_RESP_WIFI_GET_MAC, req_mac, req_mac_len, 1500, &resp_code) &&
        resp_code == 0) {
        size_t pos = 0;
        while (pos < s_wait_payload_len) {
            uint64_t tag = 0;
            if (!pb_get_varint(s_wait_payload, s_wait_payload_len, &pos, &tag)) break;
            uint32_t field = (uint32_t)(tag >> 3);
            uint8_t  wtype = (uint8_t)(tag & 0x7u);
            if (field == 1 && wtype == 2) {
                uint64_t mlen = 0;
                if (pb_get_varint(s_wait_payload, s_wait_payload_len, &pos, &mlen) &&
                    mlen == 6 && pos + 6 <= s_wait_payload_len) {
                    mem_copy(s_status.mac, s_wait_payload + pos, 6);
                }
                break;
            }
            if (!pb_skip_field(s_wait_payload, s_wait_payload_len, &pos, wtype)) break;
        }
    }

    s_wifi_inited     = true;
    s_status.hw_ready = true;
    s_status.state    = WIFI_STATE_DISCONNECTED;

    /* Load saved credentials from /home/wifi.cfg, or recover from ESP32-C6 NVS flash */
    bool auto_c = true;
    if (load_cfg_from_fs(s_status.saved_ssid, s_saved_pass, &auto_c)) {
        s_status.has_saved = true;
        s_status.auto_connect = auto_c;
    } else if (load_cfg_from_c6_nvs_locked(s_status.saved_ssid, s_saved_pass)) {
        s_status.has_saved = true;
        s_status.auto_connect = true;
        save_cfg_to_fs(s_status.saved_ssid, s_saved_pass, true);
    } else {
        s_status.has_saved = false;
        s_status.auto_connect = true;
    }

    ksnprintf(s_status.last_msg, sizeof s_status.last_msg,
              "Ready (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
              s_status.mac[0], s_status.mac[1], s_status.mac[2],
              s_status.mac[3], s_status.mac[4], s_status.mac[5]);
    kprintf("wifi: ESP32-C6 STA initialized (%s)\n", s_status.last_msg);
    mutex_unlock(&s_wifi_lock);
    return true;
}

int wifi_scan(wifi_ap_t *out_aps, int max_aps)
{
    if (!wifi_init()) return -1;

    mutex_lock(&s_wifi_lock);
    wifi_state_t prev_state = s_status.state;
    if (prev_state != WIFI_STATE_CONNECTED) {
        s_status.state = WIFI_STATE_SCANNING;
    }
    set_msg("Scanning nearby Wi-Fi networks...");

    /* Req_WifiScanStart: field 2 (block = 1), field 3 (config_set = 0) */
    uint8_t req_scan[16];
    size_t req_scan_len = 0;
    req_scan_len += pb_put_uint(req_scan + req_scan_len, 2, 1);
    req_scan_len += pb_put_uint(req_scan + req_scan_len, 3, 0);

    int32_t resp_code = -1;
    if (!rpc_call_locked(RPC_REQ_WIFI_SCAN_START, RPC_RESP_WIFI_SCAN_START,
                         req_scan, req_scan_len, 8000, &resp_code) || resp_code != 0) {
        s_status.state = (prev_state == WIFI_STATE_CONNECTED) ? WIFI_STATE_CONNECTED : WIFI_STATE_DISCONNECTED;
        set_msg("Scan failed or timed out");
        mutex_unlock(&s_wifi_lock);
        return -1;
    }

    /* Req_WifiScanGetApRecords: field 1 (number = WIFI_MAX_SCAN_APS) */
    uint8_t req_recs[8];
    size_t req_recs_len = pb_put_uint(req_recs, 1, WIFI_MAX_SCAN_APS);
    s_scan_count = 0;

    if (rpc_call_locked(RPC_REQ_WIFI_SCAN_GET_AP_RECS, RPC_RESP_WIFI_SCAN_GET_AP_RECS,
                        req_recs, req_recs_len, 4000, &resp_code) && resp_code == 0) {
        const uint8_t *buf = s_wait_payload;
        size_t len = s_wait_payload_len;
        size_t pos = 0;
        while (pos < len && s_scan_count < WIFI_MAX_SCAN_APS) {
            uint64_t tag = 0;
            if (!pb_get_varint(buf, len, &pos, &tag)) break;
            uint32_t field = (uint32_t)(tag >> 3);
            uint8_t  wtype = (uint8_t)(tag & 0x7u);
            if (field == 3 && wtype == 2) {
                uint64_t rlen = 0;
                if (!pb_get_varint(buf, len, &pos, &rlen) || pos + rlen > len) break;
                wifi_ap_t rec;
                pb_parse_ap_record(buf + pos, (size_t)rlen, &rec);
                pos += (size_t)rlen;
                if (!rec.ssid[0]) continue; /* skip hidden empty SSIDs */

                if (str_eq(rec.ssid, "shado")) {
                    kprintf("wifi: scan shado bssid=%02X:%02X:%02X:%02X:%02X:%02X ch=%u rssi=%d auth=%u\n",
                            rec.bssid[0], rec.bssid[1], rec.bssid[2],
                            rec.bssid[3], rec.bssid[4], rec.bssid[5],
                            (unsigned)rec.channel, (int)rec.rssi, (unsigned)rec.authmode);
                }

                rec.saved     = s_status.has_saved && str_eq(rec.ssid, s_status.saved_ssid);
                rec.connected = (prev_state == WIFI_STATE_CONNECTED) && str_eq(rec.ssid, s_status.ssid);

                /* De-duplicate by SSID, keeping strongest RSSI */
                int dup = -1;
                for (int i = 0; i < s_scan_count; i++) {
                    if (str_eq(s_scan_aps[i].ssid, rec.ssid)) { dup = i; break; }
                }
                if (dup >= 0) {
                    if (rec.rssi > s_scan_aps[dup].rssi) s_scan_aps[dup] = rec;
                } else {
                    s_scan_aps[s_scan_count++] = rec;
                }
            } else {
                if (!pb_skip_field(buf, len, &pos, wtype)) break;
            }
        }
    }

    /* Sort by descending RSSI (strongest first, connected/saved prioritized on ties) */
    for (int i = 1; i < s_scan_count; i++) {
        wifi_ap_t tmp = s_scan_aps[i];
        int j = i - 1;
        while (j >= 0 && tmp.rssi > s_scan_aps[j].rssi) {
            s_scan_aps[j + 1] = s_scan_aps[j];
            j--;
        }
        s_scan_aps[j + 1] = tmp;
    }

    s_status.state = (s_status.state == WIFI_STATE_CONNECTED || prev_state == WIFI_STATE_CONNECTED)
                     ? WIFI_STATE_CONNECTED : WIFI_STATE_DISCONNECTED;
    ksnprintf(s_status.last_msg, sizeof s_status.last_msg, "Scan complete: %d networks found", s_scan_count);

    int n_out = s_scan_count;
    if (out_aps && max_aps > 0) {
        if (n_out > max_aps) n_out = max_aps;
        for (int i = 0; i < n_out; i++) out_aps[i] = s_scan_aps[i];
    }
    mutex_unlock(&s_wifi_lock);
    return n_out;
}

bool wifi_connect(const char *ssid, const char *password, bool save)
{
    if (!ssid || !ssid[0]) return false;
    if (!wifi_init()) return false;

    mutex_lock(&s_wifi_lock);
    s_manual_disconnect = false;

    /* Always disconnect cleanly first and drain any asynchronous disconnect events from ESP32-C6 */
    rpc_call_locked(RPC_REQ_WIFI_DISCONNECT, RPC_RESP_WIFI_DISCONNECT, NULL, 0, 1000, NULL);
    for (int i = 0; i < 25; i++) {
        sdio_poll_rx_locked();
        task_sleep(10);
    }

    if (save) {
        str_copy(s_status.saved_ssid, sizeof s_status.saved_ssid, ssid);
        str_copy(s_saved_pass, sizeof s_saved_pass, password ? password : "");
        s_status.has_saved = true;
        s_status.auto_connect = true;
        save_cfg_to_fs(s_status.saved_ssid, s_saved_pass, s_status.auto_connect);
    }

    if (!set_c6_sta_config_locked(ssid, password ? password : "")) {
        s_status.state = WIFI_STATE_ERROR;
        set_msg("Failed to set STA config on ESP32-C6");
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    /* Drain any late events before resetting state and starting esp_wifi_connect */
    sdio_poll_rx_locked();
    str_copy(s_status.ssid, sizeof s_status.ssid, ssid);
    mem_zero(s_status.ip, 4);
    mem_zero(s_status.netmask, 4);
    mem_zero(s_status.gateway, 4);
    s_status.disconnect_reason = 0;
    s_status.state = WIFI_STATE_CONNECTING;
    ksnprintf(s_status.last_msg, sizeof s_status.last_msg, "Connecting to %s...", ssid);

    int32_t resp_code = -1;
    if (!rpc_call_locked(RPC_REQ_WIFI_CONNECT, RPC_RESP_WIFI_CONNECT, NULL, 0, 3000, &resp_code) ||
        resp_code != 0) {
        s_status.state = WIFI_STATE_DISCONNECTED;
        set_msg("esp_wifi_connect failed");
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    /* If a stale disconnect event arrived during RPC_REQ_WIFI_CONNECT, clear it now */
    if (s_status.state != WIFI_STATE_CONNECTED) {
        s_status.disconnect_reason = 0;
        s_status.state = WIFI_STATE_CONNECTING;
    }

    /* Wait up to 16s for association + DHCP IP assignment */
    int retries = 0;
    uint64_t connect_sent_us = hal_time_now_us();
    uint64_t deadline = connect_sent_us + 16000000ULL;
    while (hal_time_now_us() < deadline) {
        sdio_poll_rx_locked();
        if (s_status.state == WIFI_STATE_CONNECTED) {
            if (s_dhcp_state == 3 && s_status.ip[0] != 0) {
                break;
            }
            if ((s_dhcp_state == 1 || s_dhcp_state == 2) &&
                hal_time_now_us() - s_dhcp_last_tx_us > 1200000ULL) {
                s_dhcp_state = 1;
                dhcp_send_discover();
            }
        } else if (s_status.state == WIFI_STATE_DISCONNECTED && s_status.disconnect_reason != 0) {
            uint64_t elapsed = hal_time_now_us() - connect_sent_us;
            if (elapsed < 450000ULL) {
                /* Stale disconnect event from prior disconnect; keep waiting for active scan */
                s_status.disconnect_reason = 0;
                s_status.state = WIFI_STATE_CONNECTING;
            } else if (retries < 2 && hal_time_now_us() + 4000000ULL < deadline) {
                retries++;
                for (int i = 0; i < 20; i++) {
                    sdio_poll_rx_locked();
                    task_sleep(10);
                }
                s_status.disconnect_reason = 0;
                s_status.state = WIFI_STATE_CONNECTING;
                ksnprintf(s_status.last_msg, sizeof s_status.last_msg,
                          "Retrying %s (%d/2)...", ssid, retries);
                rpc_call_locked(RPC_REQ_WIFI_CONNECT, RPC_RESP_WIFI_CONNECT, NULL, 0, 2000, NULL);
                if (s_status.state != WIFI_STATE_CONNECTED) {
                    s_status.disconnect_reason = 0;
                    s_status.state = WIFI_STATE_CONNECTING;
                }
                connect_sent_us = hal_time_now_us();
                continue;
            } else {
                break;
            }
        }
        task_sleep(20);
    }

    /* Refresh RSSI & channel from StaGetApInfo if connected */
    if (s_status.state == WIFI_STATE_CONNECTED) {
        if (rpc_call_locked(RPC_REQ_WIFI_STA_GET_AP_INFO, RPC_RESP_WIFI_STA_GET_AP_INFO,
                            NULL, 0, 1000, &resp_code) && resp_code == 0) {
            size_t pos = 0;
            while (pos < s_wait_payload_len) {
                uint64_t tag = 0;
                if (!pb_get_varint(s_wait_payload, s_wait_payload_len, &pos, &tag)) break;
                uint32_t field = (uint32_t)(tag >> 3);
                uint8_t  wtype = (uint8_t)(tag & 0x7u);
                if (field == 2 && wtype == 2) {
                    uint64_t rlen = 0;
                    if (pb_get_varint(s_wait_payload, s_wait_payload_len, &pos, &rlen) &&
                        pos + rlen <= s_wait_payload_len) {
                        wifi_ap_t ap;
                        pb_parse_ap_record(s_wait_payload + pos, (size_t)rlen, &ap);
                        s_status.rssi = ap.rssi;
                        if (ap.channel) s_status.channel = ap.channel;
                    }
                    break;
                }
                if (!pb_skip_field(s_wait_payload, s_wait_payload_len, &pos, wtype)) break;
            }
        }
        sdio_poll_rx_locked();
    }

    bool ok = (s_status.state == WIFI_STATE_CONNECTED);
    mutex_unlock(&s_wifi_lock);
    return ok;
}

bool wifi_disconnect(void)
{
    if (!wifi_init()) return false;

    mutex_lock(&s_wifi_lock);
    s_manual_disconnect = true;
    int32_t resp_code = -1;
    rpc_call_locked(RPC_REQ_WIFI_DISCONNECT, RPC_RESP_WIFI_DISCONNECT, NULL, 0, 2000, &resp_code);
    for (int i = 0; i < 15; i++) {
        sdio_poll_rx_locked();
        task_sleep(10);
    }
    s_status.state = WIFI_STATE_DISCONNECTED;
    mem_zero(s_status.ip, 4);
    mem_zero(s_status.netmask, 4);
    mem_zero(s_status.gateway, 4);
    s_dhcp_state = 0;
    set_msg("Disconnected by user");
    mutex_unlock(&s_wifi_lock);
    return true;
}

void wifi_get_status(wifi_status_t *out)
{
    if (!out) return;
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    if (s_sdio_ready) {
        sdio_poll_rx_locked();
        if (s_status.state == WIFI_STATE_CONNECTED && s_status.ip[0] == 0 &&
            (s_dhcp_state == 1 || s_dhcp_state == 2) &&
            hal_time_now_us() - s_dhcp_last_tx_us > 1500000ULL) {
            s_dhcp_state = 1;
            dhcp_send_discover();
        }
    }
    *out = s_status;
    mutex_unlock(&s_wifi_lock);
}

bool wifi_load_saved(char *ssid_out, char *pass_out, bool *auto_conn_out)
{
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    if (!s_status.has_saved) {
        bool auto_c = true;
        if (load_cfg_from_fs(s_status.saved_ssid, s_saved_pass, &auto_c)) {
            s_status.has_saved = true;
            s_status.auto_connect = auto_c;
        }
    }
    bool ok = s_status.has_saved;
    if (ok) {
        if (ssid_out) str_copy(ssid_out, WIFI_SSID_MAX + 1, s_status.saved_ssid);
        if (pass_out) str_copy(pass_out, WIFI_PASS_MAX + 1, s_saved_pass);
        if (auto_conn_out) *auto_conn_out = s_status.auto_connect;
    }
    mutex_unlock(&s_wifi_lock);
    return ok;
}

bool wifi_save_config(const char *ssid, const char *password, bool auto_connect)
{
    if (!ssid || !ssid[0]) return false;
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    str_copy(s_status.saved_ssid, sizeof s_status.saved_ssid, ssid);
    str_copy(s_saved_pass, sizeof s_saved_pass, password ? password : "");
    s_status.has_saved = true;
    s_status.auto_connect = auto_connect;
    bool ok = save_cfg_to_fs(s_status.saved_ssid, s_saved_pass, auto_connect);
    if (s_wifi_inited) {
        set_c6_sta_config_locked(s_status.saved_ssid, s_saved_pass);
    }
    mutex_unlock(&s_wifi_lock);
    return ok;
}

bool wifi_set_auto_connect(bool enable)
{
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    s_status.auto_connect = enable;
    if (!enable) s_manual_disconnect = true;
    else s_manual_disconnect = false;
    if (s_status.has_saved) {
        save_cfg_to_fs(s_status.saved_ssid, s_saved_pass, enable);
    }
    ksnprintf(s_status.last_msg, sizeof s_status.last_msg,
              "Auto-connect %s", enable ? "ENABLED" : "DISABLED");
    mutex_unlock(&s_wifi_lock);
    return true;
}

bool wifi_forget(void)
{
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    vfs_unlink(WIFI_CFG_PATH);
    s_status.has_saved = false;
    s_status.saved_ssid[0] = 0;
    s_saved_pass[0] = 0;
    if (s_wifi_inited) {
        set_c6_sta_config_locked("", "");
    }
    set_msg("Cleared saved Wi-Fi credentials");
    mutex_unlock(&s_wifi_lock);
    return true;
}

/* =========================================================================
 * ARP / DNS / TCP Client Public API
 * ========================================================================= */

static bool parse_ipv4_literal(const char *s, uint8_t out_ip[4])
{
    if (!s || !s[0]) return false;
    uint32_t vals[4] = {0, 0, 0, 0};
    int part = 0;
    bool has_digit = false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            vals[part] = vals[part] * 10u + (uint32_t)(c - '0');
            if (vals[part] > 255u) return false;
            has_digit = true;
        } else if (c == '.') {
            if (!has_digit || part >= 3) return false;
            part++;
            has_digit = false;
        } else {
            return false;
        }
    }
    if (part != 3 || !has_digit) return false;
    for (int i = 0; i < 4; i++) out_ip[i] = (uint8_t)vals[i];
    return true;
}

static bool arp_resolve_locked(const uint8_t target_ip[4], uint8_t out_mac[6], uint32_t timeout_ms)
{
    if (s_status.ip[0] == 0) return false;

    const uint8_t *next_hop = target_ip;
    if (((target_ip[0] ^ s_status.ip[0]) & s_status.netmask[0]) != 0 ||
        ((target_ip[1] ^ s_status.ip[1]) & s_status.netmask[1]) != 0 ||
        ((target_ip[2] ^ s_status.ip[2]) & s_status.netmask[2]) != 0 ||
        ((target_ip[3] ^ s_status.ip[3]) & s_status.netmask[3]) != 0) {
        if (s_status.gateway[0] != 0) {
            next_hop = s_status.gateway;
        }
    }

    if (arp_lookup(next_hop, out_mac)) return true;

    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;
    uint64_t last_tx  = 0;
    while (hal_time_now_us() < deadline) {
        uint64_t now = hal_time_now_us();
        if (now - last_tx >= 250000ULL) {
            arp_send_request(next_hop);
            last_tx = now;
        }
        sdio_poll_rx_locked();
        if (arp_lookup(next_hop, out_mac)) return true;
        task_sleep(5);
    }
    return false;
}

static uint16_t udp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             const uint8_t *udp_seg, size_t udp_len)
{
    uint32_t sum = 0;
    sum += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    sum += 17u; /* IPPROTO_UDP */
    sum += (uint32_t)udp_len;
    for (size_t i = 0; i + 1 < udp_len; i += 2) {
        sum += ((uint32_t)udp_seg[i] << 8) | udp_seg[i + 1];
    }
    if (udp_len & 1u) {
        sum += (uint32_t)udp_seg[udp_len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    uint16_t res = (uint16_t)(~sum & 0xFFFFu);
    return res == 0 ? 0xFFFFu : res;
}

static void dns_send_query_locked(const uint8_t dns_srv[4], const uint8_t dst_mac[6],
                                  uint16_t dst_port, const char *host)
{
    uint8_t pkt[320];
    mem_zero(pkt, sizeof pkt);

    /* Ethernet */
    mem_copy(&pkt[0], dst_mac, 6);
    mem_copy(&pkt[6], s_status.mac, 6);
    pkt[12] = 0x08u; pkt[13] = 0x00u;

    /* DNS message at pkt[42] */
    uint8_t *dns = &pkt[42];
    dns[0] = (uint8_t)(s_dns_txid >> 8);
    dns[1] = (uint8_t)(s_dns_txid & 0xFFu);
    dns[2] = 0x01u; dns[3] = 0x00u; /* Standard query, RD=1 */
    dns[4] = 0x00u; dns[5] = 0x01u; /* QDCOUNT = 1 */

    size_t dlen = 12u;
    size_t i = 0;
    while (host[i] && dlen + 64u < sizeof(pkt) - 42u) {
        size_t lpos = dlen++;
        size_t llen = 0;
        while (host[i] && host[i] != '.' && llen < 63u) {
            dns[dlen++] = (uint8_t)host[i++];
            llen++;
        }
        dns[lpos] = (uint8_t)llen;
        if (host[i] == '.') i++;
    }
    dns[dlen++] = 0u; /* root label */
    dns[dlen++] = 0x00u; dns[dlen++] = 0x01u; /* QTYPE = A */
    dns[dlen++] = 0x00u; dns[dlen++] = 0x01u; /* QCLASS = IN */

    size_t udp_len = 8u + dlen;
    size_t ip_len  = 20u + udp_len;

    uint8_t *udp = &pkt[34];
    udp[0] = 0xC0u; udp[1] = 0x35u; /* src port 49205 */
    udp[2] = (uint8_t)(dst_port >> 8);
    udp[3] = (uint8_t)(dst_port & 0xFFu);
    udp[4] = (uint8_t)(udp_len >> 8);
    udp[5] = (uint8_t)(udp_len & 0xFFu);
    uint16_t ucsum = udp_checksum(s_status.ip, dns_srv, udp, udp_len);
    udp[6] = (uint8_t)(ucsum >> 8);
    udp[7] = (uint8_t)(ucsum & 0xFFu);

    uint8_t *ip = &pkt[14];
    s_ip_id++;
    ip[0] = 0x45u;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)(ip_len & 0xFFu);
    ip[4] = (uint8_t)(s_ip_id >> 8);
    ip[5] = (uint8_t)(s_ip_id & 0xFFu);
    ip[8] = 64u;
    ip[9] = 17u; /* UDP */
    mem_copy(&ip[12], s_status.ip, 4);
    mem_copy(&ip[16], dns_srv, 4);
    uint16_t hcsum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(hcsum >> 8);
    ip[11] = (uint8_t)(hcsum & 0xFFu);

    sdio_send_packet(ESP_STA_IF, 0, pkt, 14u + ip_len);
}

bool wifi_resolve_host(const char *host, uint8_t out_ip[4])
{
    if (!host || !host[0] || !out_ip) return false;
    if (parse_ipv4_literal(host, out_ip)) return true;

    ensure_lock();
    mutex_lock(&s_wifi_lock);
    wifi_tun_resolve_cb_t tun_res = s_tun_resolve_cb;
    mutex_unlock(&s_wifi_lock);
    if (tun_res && tun_res(host, out_ip)) {
        return true;
    }

    mutex_lock(&s_wifi_lock);
    if (s_status.state != WIFI_STATE_CONNECTED || s_status.ip[0] == 0) {
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    uint8_t primary_dns[4];
    if (s_dns_ip[0] != 0) mem_copy(primary_dns, s_dns_ip, 4);
    else if (s_status.gateway[0] != 0) mem_copy(primary_dns, s_status.gateway, 4);
    else { primary_dns[0] = 8; primary_dns[1] = 8; primary_dns[2] = 8; primary_dns[3] = 8; }

    static const uint8_t dns_google[4] = {8, 8, 8, 8};
    static const uint8_t dns_cf[4]     = {1, 1, 1, 1};
    static const uint8_t mdns_ip[4]    = {224, 0, 0, 251};
    static const uint8_t mdns_mac[6]   = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB};

    uint8_t gw_mac[6];
    if (!arp_resolve_locked(primary_dns, gw_mac, 2000)) {
        mutex_unlock(&s_wifi_lock);
        if (str_eq(host, "controlplane.tailscale.com")) {
            out_ip[0] = 192; out_ip[1] = 200; out_ip[2] = 0; out_ip[3] = 101;
            return true;
        }
        return false;
    }

    bool has_dot = false;
    for (size_t i = 0; host[i]; i++) {
        if (host[i] == '.') { has_dot = true; break; }
    }
    char host_local[80];
    if (!has_dot) {
        ksnprintf(host_local, sizeof(host_local), "%s.local", host);
    }

    s_dns_txid++;
    s_dns_done = false;
    uint64_t deadline = hal_time_now_us() + 3000000ULL;
    uint64_t last_tx  = 0;
    int attempt = 0;

    while (hal_time_now_us() < deadline) {
        uint64_t now = hal_time_now_us();
        if (now - last_tx >= 550000ULL) {
            const uint8_t *srv = primary_dns;
            if (attempt == 1) srv = dns_google;
            else if (attempt == 2) srv = dns_cf;
            dns_send_query_locked(srv, gw_mac, 53u, host);
            if (!has_dot) {
                /* Also query mDNS (224.0.0.251:5353) and router DNS for <host>.local */
                dns_send_query_locked(mdns_ip, mdns_mac, 5353u, host_local);
                dns_send_query_locked(primary_dns, gw_mac, 53u, host_local);
            }
            attempt++;
            last_tx = now;
        }
        sdio_poll_rx_locked();
        if (s_dns_done) {
            mem_copy(out_ip, s_dns_resolved_ip, 4);
            mutex_unlock(&s_wifi_lock);
            return true;
        }
        task_sleep(10);
    }

    mutex_unlock(&s_wifi_lock);
    if (str_eq(host, "controlplane.tailscale.com")) {
        out_ip[0] = 192; out_ip[1] = 200; out_ip[2] = 0; out_ip[3] = 101;
        return true;
    }
    return false;
}

bool wifi_tcp_connect_sock(int sock, const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    if (sock < 0 || sock >= WIFI_MAX_TCP_SOCKETS || !ip || !ip[0] || port == 0) return false;
    ensure_lock();
    mutex_lock(&s_wifi_lock);

    if (s_status.state != WIFI_STATE_CONNECTED || s_status.ip[0] == 0) {
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    tcp_conn_t *c = &s_tcp_conns[sock];
    if (!c->rx_buf) {
        c->rx_buf = (uint8_t *)kmalloc_psram(TCP_RX_BUF_SIZE);
        if (!c->rx_buf) c->rx_buf = (uint8_t *)kmalloc(TCP_RX_BUF_SIZE);
        if (!c->rx_buf) {
            mutex_unlock(&s_wifi_lock);
            return false;
        }
    }

    /* Close any previous active connection on this socket */
    if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT) {
        tcp_send_segment_locked(c, TCP_RST | TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
    }
    c->state = TCP_STATE_CLOSED;

    uint8_t dst_mac[6];
    mem_zero(dst_mac, 6);
    if (is_tailscale_ip(ip)) {
        if (!s_tun_tx_cb || s_tun_ip[0] == 0) {
            mutex_unlock(&s_wifi_lock);
            return false;
        }
    } else if (!arp_resolve_locked(ip, dst_mac, 2500)) {
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    mem_copy(c->remote_ip, ip, 4);
    mem_copy(c->remote_mac, dst_mac, 6);
    s_next_local_port = (uint16_t)(49152u + ((s_next_local_port - 49152u + 1u + (hal_time_now_us() & 0x7Fu)) & 0x3FFFu));
    c->local_port = s_next_local_port;
    c->remote_port = port;
    c->snd_una = (uint32_t)(hal_time_now_us() ^ (0x5A3C96E1u + (uint32_t)sock * 0x13579BDFu));
    c->snd_nxt = c->snd_una + 1u;
    c->rcv_nxt = 0;
    c->rx_head = 0;
    c->rx_tail = 0;
    c->rx_count = 0;
    c->state = TCP_STATE_SYN_SENT;

    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;
    uint64_t last_syn = 0;

    while (hal_time_now_us() < deadline) {
        uint64_t now = hal_time_now_us();
        if (last_syn == 0 || (now - last_syn >= 600000ULL)) {
            tcp_send_segment_locked(c, TCP_SYN, c->snd_una, 0, NULL, 0);
            last_syn = hal_time_now_us();
        }
        sdio_poll_rx_locked();
        if (c->state == TCP_STATE_ESTABLISHED) {
            mutex_unlock(&s_wifi_lock);
            return true;
        }
        if (c->state == TCP_STATE_CLOSED) {
            break;
        }
        mutex_unlock(&s_wifi_lock);
        task_sleep(5);
        mutex_lock(&s_wifi_lock);
    }

    c->state = TCP_STATE_CLOSED;
    mutex_unlock(&s_wifi_lock);
    return false;
}

int wifi_tcp_send_sock(int sock, const void *data, size_t len, uint32_t timeout_ms)
{
    if (sock < 0 || sock >= WIFI_MAX_TCP_SOCKETS) return -1;
    if (!data || len == 0) return 0;
    ensure_lock();
    mutex_lock(&s_wifi_lock);

    tcp_conn_t *c = &s_tcp_conns[sock];
    if (c->state != TCP_STATE_ESTABLISHED) {
        mutex_unlock(&s_wifi_lock);
        return -1;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = (len - sent > 1024u) ? 1024u : (len - sent);
        uint32_t seg_seq = c->snd_nxt;
        c->snd_nxt += (uint32_t)chunk;

        uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;
        uint64_t last_tx  = 0;
        bool acked = false;

        while (hal_time_now_us() < deadline) {
            uint64_t now = hal_time_now_us();
            if (now - last_tx >= 350000ULL) {
                tcp_send_segment_locked(c, TCP_PSH | TCP_ACK, seg_seq, c->rcv_nxt, p + sent, chunk);
                last_tx = now;
            }
            sdio_poll_rx_locked();
            if ((int32_t)(c->snd_una - c->snd_nxt) >= 0) {
                acked = true;
                break;
            }
            if (c->state != TCP_STATE_ESTABLISHED && c->state != TCP_STATE_CLOSE_WAIT) {
                break;
            }
            mutex_unlock(&s_wifi_lock);
            task_sleep(2);
            mutex_lock(&s_wifi_lock);
        }

        if (!acked) {
            mutex_unlock(&s_wifi_lock);
            return sent > 0 ? (int)sent : -1;
        }
        sent += chunk;
    }

    mutex_unlock(&s_wifi_lock);
    return (int)sent;
}

int wifi_tcp_recv_sock(int sock, void *buf, size_t max_len, uint32_t timeout_ms)
{
    if (sock < 0 || sock >= WIFI_MAX_TCP_SOCKETS) return -1;
    if (!buf || max_len == 0) return 0;
    ensure_lock();
    mutex_lock(&s_wifi_lock);

    tcp_conn_t *c = &s_tcp_conns[sock];
    uint8_t *out = (uint8_t *)buf;
    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;

    for (;;) {
        if (s_sdio_ready) {
            sdio_poll_rx_locked();
        }
        if (c->rx_count > 0 && c->rx_buf) {
            size_t prev_free = (c->rx_count < TCP_RX_BUF_SIZE) ? (TCP_RX_BUF_SIZE - c->rx_count) : 0u;
            size_t n = (c->rx_count > max_len) ? max_len : c->rx_count;
            for (size_t i = 0; i < n; i++) {
                out[i] = c->rx_buf[c->rx_tail];
                c->rx_tail = (c->rx_tail + 1u) % TCP_RX_BUF_SIZE;
            }
            c->rx_count -= n;
            size_t new_free = TCP_RX_BUF_SIZE - c->rx_count;
            if (c->state == TCP_STATE_ESTABLISHED && prev_free < 16384u && new_free >= 16384u) {
                tcp_send_segment_locked(c, TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
            }
            mutex_unlock(&s_wifi_lock);
            return (int)n;
        }
        if (c->state != TCP_STATE_ESTABLISHED) {
            mutex_unlock(&s_wifi_lock);
            return -1;
        }
        if (timeout_ms == 0 || hal_time_now_us() >= deadline) {
            mutex_unlock(&s_wifi_lock);
            return 0;
        }
        mutex_unlock(&s_wifi_lock);
        task_sleep(4);
        mutex_lock(&s_wifi_lock);
    }
}

void wifi_tcp_close_sock(int sock)
{
    if (sock < 0 || sock >= WIFI_MAX_TCP_SOCKETS) return;
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    tcp_conn_t *c = &s_tcp_conns[sock];
    if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT) {
        tcp_send_segment_locked(c, TCP_FIN | TCP_ACK, c->snd_nxt, c->rcv_nxt, NULL, 0);
        c->snd_nxt++;
        for (int i = 0; i < 10; i++) {
            sdio_poll_rx_locked();
            task_sleep(5);
        }
    }
    c->state = TCP_STATE_CLOSED;
    c->rx_head = c->rx_tail = c->rx_count = 0;
    mutex_unlock(&s_wifi_lock);
}

bool wifi_tcp_is_connected_sock(int sock)
{
    if (sock < 0 || sock >= WIFI_MAX_TCP_SOCKETS) return false;
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    if (s_sdio_ready) sdio_poll_rx_locked();
    tcp_conn_t *c = &s_tcp_conns[sock];
    bool ok = (c->state == TCP_STATE_ESTABLISHED || c->rx_count > 0);
    mutex_unlock(&s_wifi_lock);
    return ok;
}

bool wifi_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    return wifi_tcp_connect_sock(0, ip, port, timeout_ms);
}

int wifi_tcp_send(const void *data, size_t len, uint32_t timeout_ms)
{
    return wifi_tcp_send_sock(0, data, len, timeout_ms);
}

int wifi_tcp_recv(void *buf, size_t max_len, uint32_t timeout_ms)
{
    return wifi_tcp_recv_sock(0, buf, max_len, timeout_ms);
}

void wifi_tcp_close(void)
{
    wifi_tcp_close_sock(0);
}

bool wifi_tcp_is_connected(void)
{
    return wifi_tcp_is_connected_sock(0);
}

bool wifi_udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
                   const void *data, size_t len)
{
    if (!dst_ip || !dst_ip[0] || dst_port == 0 || len > 1440u) return false;
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    if (s_status.state != WIFI_STATE_CONNECTED || s_status.ip[0] == 0) {
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    uint8_t dst_mac[6];
    if (!arp_resolve_locked(dst_ip, dst_mac, 1000)) {
        mutex_unlock(&s_wifi_lock);
        return false;
    }

    static uint8_t pkt[1500];
    size_t udp_len = 8u + len;
    size_t ip_len  = 20u + udp_len;
    mem_zero(pkt, 14u + 20u + 8u);

    mem_copy(&pkt[0], dst_mac, 6);
    mem_copy(&pkt[6], s_status.mac, 6);
    pkt[12] = 0x08u; pkt[13] = 0x00u;

    uint8_t *ip = &pkt[14];
    s_ip_id++;
    ip[0] = 0x45u;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)(ip_len & 0xFFu);
    ip[4] = (uint8_t)(s_ip_id >> 8);
    ip[5] = (uint8_t)(s_ip_id & 0xFFu);
    ip[6] = 0x40u;
    ip[8] = 64u;
    ip[9] = 17u; /* UDP */
    mem_copy(&ip[12], s_status.ip, 4);
    mem_copy(&ip[16], dst_ip, 4);
    uint16_t hcsum = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(hcsum >> 8);
    ip[11] = (uint8_t)(hcsum & 0xFFu);

    uint8_t *udp = &pkt[34];
    udp[0] = (uint8_t)(src_port >> 8);
    udp[1] = (uint8_t)(src_port & 0xFFu);
    udp[2] = (uint8_t)(dst_port >> 8);
    udp[3] = (uint8_t)(dst_port & 0xFFu);
    udp[4] = (uint8_t)(udp_len >> 8);
    udp[5] = (uint8_t)(udp_len & 0xFFu);
    udp[6] = 0u;
    udp[7] = 0u;

    if (len > 0 && data) {
        mem_copy(&pkt[42], data, len);
    }

    uint16_t ucsum = udp_checksum(s_status.ip, dst_ip, udp, udp_len);
    udp[6] = (uint8_t)(ucsum >> 8);
    udp[7] = (uint8_t)(ucsum & 0xFFu);

    bool ok = sdio_send_packet(ESP_STA_IF, 0, pkt, 14u + ip_len);
    mutex_unlock(&s_wifi_lock);
    return ok;
}

void wifi_set_udp_rx_callback(uint16_t port, wifi_udp_rx_cb_t cb)
{
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    s_udp_rx_port = port;
    s_udp_rx_cb   = cb;
    mutex_unlock(&s_wifi_lock);
}

void wifi_set_tun_callbacks(const uint8_t tun_ip[4], wifi_tun_tx_cb_t tx_cb,
                            wifi_tun_resolve_cb_t resolve_cb)
{
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    if (tun_ip) mem_copy(s_tun_ip, tun_ip, 4);
    else mem_zero(s_tun_ip, 4);
    s_tun_tx_cb      = tx_cb;
    s_tun_resolve_cb = resolve_cb;
    mutex_unlock(&s_wifi_lock);
}

void wifi_tun_inject_rx_ipv4(const uint8_t *ipv4_pkt, size_t len)
{
    if (!ipv4_pkt || len < 20u) return;
    ensure_lock();
    mutex_lock(&s_wifi_lock);
    handle_ipv4_rx_locked(NULL, 0, ipv4_pkt, len, true);
    mutex_unlock(&s_wifi_lock);
}


/* =========================================================================
 * Background Wi-Fi Daemon Task (Boot Auto-Connect + RX Polling + Reconnect)
 * ========================================================================= */

static void wifi_daemon_task(void *arg)
{
    (void)arg;
    task_sleep(1500);
    if (!wifi_init()) return;

    /* Auto-connect at boot if saved credentials exist and auto_connect is enabled */
    char ssid[WIFI_SSID_MAX + 1] = "";
    char pass[WIFI_PASS_MAX + 1] = "";
    bool auto_c = false;
    if (wifi_load_saved(ssid, pass, &auto_c) && auto_c && ssid[0]) {
        kprintf("wifi: auto-connecting to saved AP \"%s\"...\n", ssid);
        if (wifi_connect(ssid, pass, false)) {
            kprintf("wifi: connected to \"%s\" (%u.%u.%u.%u)\n",
                    s_status.ssid, s_status.ip[0], s_status.ip[1], s_status.ip[2], s_status.ip[3]);
        }
    }

    uint32_t tick = 0;
    for (;;) {
        mutex_lock(&s_wifi_lock);
        if (s_sdio_ready) {
            sdio_poll_rx_locked();
            if (s_status.state == WIFI_STATE_CONNECTED && s_dhcp_state == 1 &&
                hal_time_now_us() - s_dhcp_last_tx_us > 2500000ULL) {
                dhcp_send_discover();
            }
        }
        bool auth_failed = (s_status.disconnect_reason == 15u ||
                            s_status.disconnect_reason == 202u ||
                            s_status.disconnect_reason == 204u);
        bool need_reconnect = (s_wifi_inited &&
                               s_status.state == WIFI_STATE_DISCONNECTED &&
                               s_status.has_saved &&
                               s_status.auto_connect &&
                               !s_manual_disconnect &&
                               !auth_failed &&
                               (tick > 0 && (tick % 300u) == 0));
        mutex_unlock(&s_wifi_lock);

        if (need_reconnect) {
            wifi_connect(s_status.saved_ssid, s_saved_pass, false);
        }

        tick++;
        task_sleep(100);
    }
}

void wifi_boot_start(void)
{
    ensure_lock();
    if (s_daemon_started) return;
    s_daemon_started = true;
    task_create_stack("wifid", wifi_daemon_task, NULL, TASK_PRIO_DEFAULT, 16384);
}
