#pragma once
#include <m5os/types.h>

#define TAILSCALE_CFG_PATH "/home/tailscale.cfg"
#define TAILSCALE_MAX_PEERS 64

typedef enum {
    TS_STATE_STOPPED      = 0,
    TS_STATE_CONNECTING   = 1,
    TS_STATE_NEEDS_LOGIN  = 2,
    TS_STATE_RUNNING      = 3,
    TS_STATE_ERROR        = 4,
} ts_state_t;

typedef struct {
    bool     valid;
    char     hostname[48];
    uint8_t  tailscale_ip[4];
    uint8_t  node_pub[32];
    uint8_t  disco_pub[32];
    uint8_t  endpoint_ip[4];
    uint16_t endpoint_port;
    bool     online;
    bool     wg_established;
    uint32_t last_rtt_ms;
    uint32_t rx_packets;
    uint32_t tx_packets;
} ts_peer_info_t;

typedef struct {
    ts_state_t     state;
    uint8_t        my_ip[4];
    char           hostname[32];
    char           auth_url[160];
    char           last_msg[80];
    int            peer_count;
    ts_peer_info_t peers[TAILSCALE_MAX_PEERS];
} ts_status_t;

/* Auto-start background Tailscale daemon on boot once Wi-Fi connects */
void tailscale_boot_start(void);

/* Start background Tailscale daemon and connect to control plane + WireGuard data plane */
bool tailscale_up(const char *auth_key);

/* Stop Tailscale tunnel */
void tailscale_down(void);

/* Query current Tailscale status and peer table */
void tailscale_get_status(ts_status_t *out);

/* Ping a Tailscale peer by hostname or 100.x.y.z IP via Disco + WireGuard ICMP. Returns RTT in ms or <0 on timeout. */
int  tailscale_ping(const char *target, uint8_t out_peer_ip[4], uint8_t out_ep_ip[4], uint16_t *out_ep_port);

/* CLI entry point (`tailscale [up|down|status|ip|ping|gui]`) */
void tailscale_cmd_main(int argc, char **argv);

/* Full-screen Graphical Tailscale Manager (`tailscale` / `run tailscale`) */
void tailscale_app_main(void);
