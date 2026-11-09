/* WireGuard tunnel for the MQTT link.
 *
 * The device keeps its normal Wi-Fi default route. Only destinations inside
 * the configured tunnel address/netmask are routed through the WireGuard
 * interface, so the web interface stays reachable on the LAN even while the
 * broker is reached across the VPN. See the README section "WireGuard" for the
 * consequences of that choice.
 *
 * The library underneath (esphome/wireguard, in wireguard/) is a single-peer,
 * IPv4-only port of esp_wireguard. All of its calls therefore live on one
 * task, the same way the CAN stack keeps one owner for the bus.
 */
#ifndef O3E_WG_LINK_H
#define O3E_WG_LINK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WG_LINK_DISABLED = 0,
    WG_LINK_WAITING,    /* enabled, but no station address yet */
    WG_LINK_CONNECTING, /* handshake not completed */
    WG_LINK_UP,         /* peer has a valid keypair */
    WG_LINK_ERROR,
} wg_link_state_t;

typedef struct {
    bool enabled;
    wg_link_state_t state;
    bool peer_up;
    bool have_handshake;
    uint32_t handshake_age_s;
    char endpoint_ip[16];
    char error[96];
} wg_link_status_t;

/* Prepare the module and apply the stored configuration. The tunnel task is
 * created only once WireGuard is enabled, so a device that never uses it
 * pays nothing beyond the code in the image. Call before net_prov_start() so
 * the IP events can reach a task that already exists. */
void wg_link_init(void);

/* From the Wi-Fi event handler. Both only notify; the work happens on the
 * tunnel task. */
void wg_link_network_up(void);
void wg_link_network_down(void);

/* Re-read the configuration and reconnect. Safe to call while a tunnel is up. */
void wg_link_restart(void);

void wg_link_status(wg_link_status_t *out);

/* Derive this device's public key from the stored private key, in the form
 * the peer's configuration expects it. Returns false when no valid private
 * key is stored. */
bool wg_link_device_public_key(char *out, size_t out_sz);

#endif /* O3E_WG_LINK_H */
