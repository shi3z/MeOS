#include <m5os/wifi.h>
#include <m5os/kprintf.h>

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

bool wifi_init(void) { return false; }
void wifi_get_diagnostics(wifi_diagnostics_t *out) { if (out) *out = (wifi_diagnostics_t){0}; }
void wifi_reset_diagnostics(void) { }
void wifi_boot_start(void) { }
int  wifi_scan(wifi_ap_t *out_aps, int max_aps) { (void)out_aps; (void)max_aps; return -1; }
bool wifi_connect(const char *ssid, const char *password, bool save) { (void)ssid; (void)password; (void)save; return false; }
bool wifi_disconnect(void) { return false; }
bool wifi_load_saved(char *ssid_out, char *pass_out, bool *auto_conn_out) { (void)ssid_out; (void)pass_out; (void)auto_conn_out; return false; }
bool wifi_save_config(const char *ssid, const char *password, bool auto_connect) { (void)ssid; (void)password; (void)auto_connect; return false; }
bool wifi_set_auto_connect(bool enable) { (void)enable; return false; }
bool wifi_forget(void) { return false; }

bool wifi_resolve_host(const char *host, uint8_t out_ip[4]) { (void)host; (void)out_ip; return false; }
bool wifi_tcp_connect_sock(int sock, const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) { (void)sock; (void)ip; (void)port; (void)timeout_ms; return false; }
int  wifi_tcp_send_sock(int sock, const void *data, size_t len, uint32_t timeout_ms) { (void)sock; (void)data; (void)len; (void)timeout_ms; return -1; }
int  wifi_tcp_recv_sock(int sock, void *buf, size_t max_len, uint32_t timeout_ms) { (void)sock; (void)buf; (void)max_len; (void)timeout_ms; return -1; }
void wifi_tcp_close_sock(int sock) { (void)sock; }
bool wifi_tcp_is_connected_sock(int sock) { (void)sock; return false; }

bool wifi_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) { (void)ip; (void)port; (void)timeout_ms; return false; }
int  wifi_tcp_send(const void *data, size_t len, uint32_t timeout_ms) { (void)data; (void)len; (void)timeout_ms; return -1; }
int  wifi_tcp_recv(void *buf, size_t max_len, uint32_t timeout_ms) { (void)buf; (void)max_len; (void)timeout_ms; return -1; }
void wifi_tcp_close(void) { }
bool wifi_tcp_is_connected(void) { return false; }

bool wifi_udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port, const void *data, size_t len)
{
    (void)dst_ip; (void)src_port; (void)dst_port; (void)data; (void)len;
    return false;
}
void wifi_set_udp_rx_callback(uint16_t port, wifi_udp_rx_cb_t cb) { (void)port; (void)cb; }
void wifi_set_tun_callbacks(const uint8_t tun_ip[4], wifi_tun_tx_cb_t tx_cb, wifi_tun_resolve_cb_t resolve_cb)
{
    (void)tun_ip; (void)tx_cb; (void)resolve_cb;
}
void wifi_tun_inject_rx_ipv4(const uint8_t *ipv4_pkt, size_t len) { (void)ipv4_pkt; (void)len; }

void wifi_get_status(wifi_status_t *out)
{
    if (!out) return;
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    out->state = WIFI_STATE_UNINIT;
    ksnprintf(out->last_msg, sizeof out->last_msg, "Wi-Fi driver not supported on this board");
}

