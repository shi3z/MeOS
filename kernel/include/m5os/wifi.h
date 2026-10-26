#pragma once
#include <m5os/types.h>

#define WIFI_SSID_MAX       32
#define WIFI_PASS_MAX       64
#define WIFI_MAX_SCAN_APS   32
#define WIFI_CFG_PATH       "/home/wifi.cfg"

typedef enum {
    WIFI_AUTH_OPEN          = 0,
    WIFI_AUTH_WEP           = 1,
    WIFI_AUTH_WPA_PSK       = 2,
    WIFI_AUTH_WPA2_PSK      = 3,
    WIFI_AUTH_WPA_WPA2_PSK  = 4,
    WIFI_AUTH_ENTERPRISE    = 5,
    WIFI_AUTH_WPA3_PSK      = 6,
    WIFI_AUTH_WPA2_WPA3_PSK = 7,
    WIFI_AUTH_WAPI_PSK      = 8,
    WIFI_AUTH_OWE           = 9,
} wifi_auth_mode_t;

typedef enum {
    WIFI_STATE_UNINIT       = 0,
    WIFI_STATE_DISCONNECTED = 1,
    WIFI_STATE_SCANNING     = 2,
    WIFI_STATE_CONNECTING   = 3,
    WIFI_STATE_CONNECTED    = 4,
    WIFI_STATE_ERROR        = 5,
} wifi_state_t;

typedef struct {
    char    ssid[WIFI_SSID_MAX + 1];
    uint8_t bssid[6];
    int8_t  rssi;
    uint8_t channel;
    uint8_t authmode;
    bool    saved;
    bool    connected;
} wifi_ap_t;

typedef struct {
    bool         hw_ready;
    wifi_state_t state;
    char         ssid[WIFI_SSID_MAX + 1];
    uint8_t      bssid[6];
    int8_t       rssi;
    uint8_t      channel;
    uint8_t      authmode;
    uint8_t      mac[6];
    uint8_t      ip[4];
    uint8_t      netmask[4];
    uint8_t      gateway[4];
    uint16_t     disconnect_reason;
    bool         auto_connect;
    bool         has_saved;
    char         saved_ssid[WIFI_SSID_MAX + 1];
    uint32_t     rx_packets;
    uint32_t     tx_packets;
    char         last_msg[64];
} wifi_status_t;

/* Initialize ESP32-C6 SDIO link + Wi-Fi station stack (idempotent). */
bool wifi_init(void);

/* Start background Wi-Fi daemon task and auto-connect if saved credentials exist. */
void wifi_boot_start(void);

/* Scan nearby 2.4 GHz Wi-Fi networks, sorted by strongest RSSI first. Returns count or <0 on error. */
int  wifi_scan(wifi_ap_t *out_aps, int max_aps);

/* Connect to an AP. If save == true, stores credentials in /home/wifi.cfg and ESP32-C6 NVS flash. */
bool wifi_connect(const char *ssid, const char *password, bool save);

/* Disconnect from current AP (and disable auto-reconnect until next manual connect or boot if auto_connect=false). */
bool wifi_disconnect(void);

/* Read current Wi-Fi status snapshot. */
void wifi_get_status(wifi_status_t *out);

/* Transport counters only; reset preserves pending RX fragments and protocol state. */
typedef struct {
    uint32_t rx_frames, rx_partials, rx_bad_headers, rx_max_pending;
    uint32_t rx_reg_errors, rx_io_errors, rx_plan_errors;
    uint32_t tx_no_credit, tx_io_errors;
    uint32_t icmp_requests, icmp_replies, icmp_reply_failures;
    uint32_t poll_max_gap_us, poll_max_duration_us, bus_max_wait_us;
} wifi_diagnostics_t;
void wifi_get_diagnostics(wifi_diagnostics_t *out);
void wifi_reset_diagnostics(void);

/* Saved credential helpers (/home/wifi.cfg + ESP32-C6 NVS flash fallback). */
bool wifi_load_saved(char *ssid_out, char *pass_out, bool *auto_conn_out);
bool wifi_save_config(const char *ssid, const char *password, bool auto_connect);
bool wifi_set_auto_connect(bool enable);
bool wifi_forget(void);

const char *wifi_auth_name(uint8_t authmode);
const char *wifi_state_name(wifi_state_t state);

/* Network / TCP client helpers over ESP_STA_IF (supports up to WIFI_MAX_TCP_SOCKETS simultaneous connections) */
#define WIFI_MAX_TCP_SOCKETS 4

bool wifi_resolve_host(const char *host, uint8_t out_ip[4]);
bool wifi_tcp_connect_sock(int sock, const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);
int  wifi_tcp_send_sock(int sock, const void *data, size_t len, uint32_t timeout_ms);
int  wifi_tcp_recv_sock(int sock, void *buf, size_t max_len, uint32_t timeout_ms);
void wifi_tcp_close_sock(int sock);
bool wifi_tcp_is_connected_sock(int sock);

bool wifi_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);
int  wifi_tcp_send(const void *data, size_t len, uint32_t timeout_ms);
int  wifi_tcp_recv(void *buf, size_t max_len, uint32_t timeout_ms);
void wifi_tcp_close(void);
bool wifi_tcp_is_connected(void);

/* UDP datagram & virtual tunnel (Tailscale 100.64.0.0/10) hooks */
typedef void (*wifi_udp_rx_cb_t)(const uint8_t src_ip[4], uint16_t src_port,
                                 uint16_t dst_port, const uint8_t *data, size_t len);
typedef bool (*wifi_tun_tx_cb_t)(const uint8_t *ipv4_pkt, size_t len);
typedef bool (*wifi_tun_resolve_cb_t)(const char *host, uint8_t out_ip[4]);

bool wifi_udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
                   const void *data, size_t len);
void wifi_set_udp_rx_callback(uint16_t port, wifi_udp_rx_cb_t cb);
void wifi_set_tun_callbacks(const uint8_t tun_ip[4], wifi_tun_tx_cb_t tx_cb,
                            wifi_tun_resolve_cb_t resolve_cb);
void wifi_tun_inject_rx_ipv4(const uint8_t *ipv4_pkt, size_t len);
