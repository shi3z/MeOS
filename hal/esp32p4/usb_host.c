/*
 * USB host on the ESP32-P4's high-speed OTG controller (Synopsys DWC2), polled, FreeRTOS-free.
 *
 * What is IDF's and what is ours:
 *  - the PHY (UTMI) power-up and clock gating: IDF's usb_new_phy(), which is plain register code;
 *  - register access and the DMA descriptor format: IDF's hal/usb_dwc_hal.h (HAL = no RTOS inside);
 *  - NOT used: IDF's USB host stack (usb_host, hcd_dwc, hub, enum) - tasks, queues, mutexes.
 * The sequencing (power, FIFO sizes, control stages with PID toggling) follows components/usb/hcd_dwc.c
 * of ESP-IDF v5.4.4, minus everything about queues, multiple pipes and hubs.
 *
 * DMA and the cache: the controller reads and writes internal RAM by DMA, the CPU goes through the L1
 * data cache.  So descriptors and buffers are cache-line aligned, written back before the controller
 * runs (C2M) and invalidated before the CPU reads what it wrote (M2C).
 */
#include <m5os/hal_usbh.h>
#include <m5os/hal_time.h>
#include <m5os/hal_gpio.h>
#include <m5os/kprintf.h>
#include <string.h>

#include "esp_idf_version.h"
#include "hal/usb_dwc_hal.h"
#include "esp_private/usb_phy.h"
#include "esp_cache.h"

#define USB_PORT            0           /* ESP32-P4: port 0 is the high-speed core with the UTMI PHY */
#define MAX_CHANNELS        16          /* DWC2 has up to 16 host channels */
#define FIFO_DEPTH_HS_LINES 1024        /* hcd_dwc.c: otg_dfifo_depth for a core with a HS PHY */
#define CACHE_LINE          64u
#define QTD_LIST_ALIGN      512         /* USB_DWC_QTD_LIST_MEM_ALIGN */
#define QTDS_PER_CONTROL    3           /* setup, data, status */
#define MAX_DATA            256         /* largest data stage we support (config descriptors fit) */
#define STAGE_TIMEOUT_US    500000u
#define SETUP_BYTES         8

/* PIDs for the stages (hcd_dwc.c: _buffer_exec / _buffer_exec_cont) */
#define PID_DATA0  0
#define PID_DATA1  1

static usb_dwc_hal_context_t  s_hal;
static usb_dwc_hal_chan_t    *s_chan_hdls[MAX_CHANNELS];
static usb_dwc_hal_chan_t     s_chan;
static bool                   s_chan_open;
static usb_phy_handle_t       s_phy;
static bool                   s_inited;
static uint16_t               s_mps;

static uint8_t s_qtd[QTD_LIST_ALIGN] __attribute__((aligned(QTD_LIST_ALIGN)));         /* only the first 24 bytes are used */
static uint8_t s_setup[CACHE_LINE]   __attribute__((aligned(CACHE_LINE)));
static uint8_t s_data[MAX_DATA]      __attribute__((aligned(CACHE_LINE)));

static void cache_write_back(void *p, size_t n) { esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
static void cache_invalidate(void *p, size_t n) { esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C); }

bool hal_usbh_init(void)
{
    if (s_inited) return true;
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
        .target     = USB_PHY_TARGET_UTMI,
#else
        .target     = USB_PHY_TARGET_INT,
#endif
        .otg_mode   = USB_OTG_MODE_HOST,
        .otg_speed  = USB_PHY_SPEED_UNDEFINED,         /* host: the speed is whatever gets plugged in */
    };
    if (usb_new_phy(&phy_cfg, &s_phy) != ESP_OK) { kprintf("usbh: PHY power-up failed\n"); return false; }

    usb_dwc_hal_init(&s_hal, USB_PORT);
    s_hal.channels.hdls = s_chan_hdls;
    if (s_hal.constant_config.chan_num_total > MAX_CHANNELS) { kprintf("usbh: unexpected channel count\n"); return false; }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
    /* FIFO split = "balanced" in hcd_dwc.c.  Each unit is one 32-bit line. */
    usb_dwc_hal_fifo_config_t fifo;
    fifo.nptx_fifo_lines = FIFO_DEPTH_HS_LINES / 4;
    fifo.ptx_fifo_lines  = FIFO_DEPTH_HS_LINES / 8;
    fifo.rx_fifo_lines   = s_hal.constant_config.fifo_size - fifo.ptx_fifo_lines - fifo.nptx_fifo_lines;
    if (!usb_dwc_hal_fifo_config_is_valid(&s_hal, &fifo)) { kprintf("usbh: FIFO config invalid\n"); return false; }
    usb_dwc_hal_set_fifo_config(&s_hal, &fifo);
#else
    (void)FIFO_DEPTH_HS_LINES;
    usb_dwc_hal_set_fifo_bias(&s_hal, USB_HAL_FIFO_BIAS_DEFAULT);
#endif

    usb_dwc_hal_port_init(&s_hal);                     /* unmask port/channel interrupt status */
    usb_dwc_hal_port_toggle_power(&s_hal, true);       /* VBUS comes from the expander (board_init); this powers the port logic */
    s_inited = true;
    return true;
}

usbh_event_t hal_usbh_poll(void)
{
    if (!s_inited) return USBH_EVENT_NONE;
    switch (usb_dwc_hal_decode_intr(&s_hal)) {
    case USB_DWC_HAL_PORT_EVENT_CONN:     return USBH_EVENT_CONNECT;
    case USB_DWC_HAL_PORT_EVENT_DISCONN:  return USBH_EVENT_DISCONNECT;
    case USB_DWC_HAL_PORT_EVENT_ENABLED:  usb_dwc_hal_port_enable(&s_hal); return USBH_EVENT_ENABLED;
    case USB_DWC_HAL_PORT_EVENT_DISABLED: return USBH_EVENT_DISABLED;
    case USB_DWC_HAL_PORT_EVENT_OVRCUR:   return USBH_EVENT_OVERCURRENT;
    default:                              return USBH_EVENT_NONE;
    }
}

void hal_usbh_debounce_done(void) { usb_dwc_hal_disable_debounce_lock(&s_hal); }
bool hal_usbh_connected(void)     { return s_inited && usb_dwc_hal_port_check_if_connected(&s_hal); }
void hal_usbh_reset(bool on)      { usb_dwc_hal_port_toggle_reset(&s_hal, on); }

usbh_speed_t hal_usbh_speed(void)
{
    switch (usb_dwc_hal_port_get_conn_speed(&s_hal)) {
    case USB_DWC_SPEED_HIGH: return USBH_SPEED_HIGH;
    case USB_DWC_SPEED_FULL: return USBH_SPEED_FULL;
    default:                 return USBH_SPEED_LOW;
    }
}

bool hal_usbh_open_control(uint8_t dev_addr, uint16_t max_packet)
{
    if (!s_inited) return false;
    if (s_chan_open) {                                  /* reconfigure: free and allocate again (the channel must be idle) */
        usb_dwc_hal_chan_free(&s_hal, &s_chan);
        s_chan_open = false;
    }
    if (!usb_dwc_hal_chan_alloc(&s_hal, &s_chan, NULL)) return false;
    usb_dwc_hal_ep_char_t ep = {
        .type = USB_DWC_XFER_TYPE_CTRL, .bEndpointAddress = 0, .mps = max_packet,
        .dev_addr = dev_addr, .ls_via_fs_hub = 0,
    };
    usb_dwc_hal_chan_set_ep_char(&s_hal, &s_chan, &ep);
    s_mps = max_packet;
    s_chan_open = true;
    return true;
}

/* Run one stage (one descriptor of the list) to completion.  Returns 0, or a negative USBH_ERR_*. */
static int run_stage(int idx, bool dir_in, int pid)
{
    usb_dwc_hal_chan_set_dir(&s_chan, dir_in);
    usb_dwc_hal_chan_set_pid(&s_chan, pid);
    usb_dwc_hal_chan_activate(&s_chan, s_qtd, QTDS_PER_CONTROL, idx);

    uint64_t start = hal_time_now_us();
    while (hal_time_now_us() - start < STAGE_TIMEOUT_US) {
        if (usb_dwc_hal_decode_intr(&s_hal) != USB_DWC_HAL_PORT_EVENT_CHAN) continue;
        usb_dwc_hal_chan_t *c;
        while ((c = usb_dwc_hal_get_chan_pending_intr(&s_hal)) != NULL) {
            if (c != &s_chan) continue;
            switch (usb_dwc_hal_chan_decode_intr(c)) {
            case USB_DWC_HAL_CHAN_EVENT_CPLT:  return 0;
            case USB_DWC_HAL_CHAN_EVENT_ERROR:
                return usb_dwc_hal_chan_get_error(c) == USB_DWC_HAL_CHAN_ERROR_STALL ? USBH_ERR_STALL : USBH_ERR_XFER;
            default: break;                             /* NONE / HALT_REQ: keep waiting */
            }
        }
    }
    usb_dwc_hal_chan_request_halt(&s_chan);             /* give up: stop the channel so it can be reused */
    start = hal_time_now_us();
    while (hal_time_now_us() - start < STAGE_TIMEOUT_US / 10) {
        if (usb_dwc_hal_decode_intr(&s_hal) != USB_DWC_HAL_PORT_EVENT_CHAN) continue;
        usb_dwc_hal_chan_t *c;
        while ((c = usb_dwc_hal_get_chan_pending_intr(&s_hal)) != NULL) if (c == &s_chan) usb_dwc_hal_chan_decode_intr(c);
        if (!s_chan.flags.active) break;
    }
    return USBH_ERR_TIMEOUT;
}

int hal_usbh_control(const uint8_t setup[8], void *data, size_t len)
{
    if (!s_chan_open) return USBH_ERR_XFER;
    bool in = setup[0] & 0x80;
    bool has_data = len > 0;
    size_t span = in ? ((len + s_mps - 1) / s_mps) * s_mps : len;      /* IN buffers must be a multiple of the packet size */
    if (span > MAX_DATA) return USBH_ERR_SIZE;

    memset(s_qtd, 0, sizeof s_qtd);
    memcpy(s_setup, setup, SETUP_BYTES);
    cache_write_back(s_setup, CACHE_LINE);
    if (has_data) {
        if (in) memset(s_data, 0, MAX_DATA); else memcpy(s_data, data, len);
        cache_write_back(s_data, MAX_DATA);
    }

    /* Descriptor 0 SETUP, 1 data (skipped if none), 2 status.  HOC = halt the channel after this descriptor, so
     * we get control back between stages and can set the direction and PID the next stage needs. */
    usb_dwc_hal_xfer_desc_fill(s_qtd, 0, s_setup, SETUP_BYTES, USB_DWC_HAL_XFER_DESC_FLAG_SETUP | USB_DWC_HAL_XFER_DESC_FLAG_HOC);
    if (has_data)
        usb_dwc_hal_xfer_desc_fill(s_qtd, 1, s_data, (int)span, (in ? USB_DWC_HAL_XFER_DESC_FLAG_IN : 0) | USB_DWC_HAL_XFER_DESC_FLAG_HOC);
    bool status_in = !(has_data && in);                  /* status stage runs opposite to the data stage; IN if there is none */
    usb_dwc_hal_xfer_desc_fill(s_qtd, 2, NULL, 0, (status_in ? USB_DWC_HAL_XFER_DESC_FLAG_IN : 0) | USB_DWC_HAL_XFER_DESC_FLAG_HOC);
    cache_write_back(s_qtd, CACHE_LINE);

    int r = run_stage(0, false, PID_DATA0);              /* setup stage is always OUT, DATA0 */
    if (r == 0 && has_data) r = run_stage(1, in, PID_DATA1);
    if (r == 0) r = run_stage(2, status_in, PID_DATA1);
    if (r != 0) return r;

    int got = 0;
    if (has_data && in) {
        cache_invalidate(s_qtd, CACHE_LINE);
        cache_invalidate(s_data, MAX_DATA);
        int rem, status;
        usb_dwc_hal_xfer_desc_parse(s_qtd, 1, &rem, &status);
        if (status != USB_DWC_HAL_XFER_DESC_STS_SUCCESS) return USBH_ERR_XFER;
        got = (int)span - rem;
        if ((size_t)got > len) got = (int)len;
        memcpy(data, s_data, (size_t)got);
    } else {
        got = (int)len;
    }
    return got;
}

void hal_usbh_dump(void)
{
    usb_dwc_dev_t *d = s_hal.dev;
    kprintf("usbh: hprt=%08x gintsts=%08x gotgctl=%08x gusbcfg=%08x hcfg=%08x\n",
            (unsigned)d->hprt_reg.val, (unsigned)d->gintsts_reg.val, (unsigned)d->gotgctl_reg.val,
            (unsigned)d->gusbcfg_reg.val, (unsigned)d->hcfg_reg.val);
}
