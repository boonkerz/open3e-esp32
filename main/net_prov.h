/* Wi-Fi provisioning and connection management.
 *
 * First boot has no credentials, so the device raises its own access point
 * with a captive portal; the browser opens the setup page by itself.  After
 * the credentials are saved the device reboots into station mode and is
 * reachable as <hostname>.local.
 *
 * If the stored network cannot be joined -- the router changed its password,
 * or was replaced -- the device falls back to the setup access point instead
 * of retrying silently.  Without that, reconfiguring means opening the boiler
 * room cabinet and plugging in a USB cable.
 */
#ifndef O3E_NET_PROV_H
#define O3E_NET_PROV_H

#include <stdbool.h>
#include <stdint.h>

/* How long to give the stored network before falling back to setup mode. */
#define NET_STA_TIMEOUT_MS  60000
#define NET_STA_RETRIES     3

typedef enum {
    NET_BOOTING = 0,
    NET_AP_SETUP,      /* serving the captive portal */
    NET_STA_CONNECTING,
    NET_STA_CONNECTED,
} net_state_t;

typedef struct {
    net_state_t state;
    char        ssid[33];
    char        ip[16];
    char        ap_ssid[33];
    int8_t      rssi;
    uint8_t     retries;
    char        bssid[18];   /* access point the station is joined to */
    uint8_t     channel;
} net_status_t;

/* Brings up Wi-Fi in whichever mode the stored configuration implies. */
void net_prov_start(void);
void net_prov_status(net_status_t *out);
bool net_prov_is_setup_mode(void);

/* Scan for nearby networks; result is a JSON array the setup page renders.
 * Caller frees. */
char *net_prov_scan_json(void);

/* True once the clock has been set from the network, so callers can tell a
 * real timestamp from seconds-since-boot. */
bool net_time_valid(void);

#endif /* O3E_NET_PROV_H */
