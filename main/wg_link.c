#include "wg_link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sodium.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "mqtt_pub.h"
#include "net_prov.h"

static const char *TAG = "wireguard";

#define WG_NOTIFY_APPLY     (1u << 0)
#define WG_NOTIFY_NET_UP    (1u << 1)
#define WG_NOTIFY_NET_DOWN  (1u << 2)

/* The library resolves the endpoint asynchronously. It returns ESP_ERR_RETRY
 * while that is in progress, which is not a failure and is retried quickly. */
#define WG_DNS_RETRY_MS      1000
/* A real failure -- wrong key, endpoint unreachable, malformed address -- gets
 * a longer pause so a misconfiguration does not turn into a busy loop. */
#define WG_ERROR_RETRY_MS   10000
/* WireGuard timestamps are TAI64N from gettimeofday(). Before SNTP has set the
 * clock every handshake carries 1970, which a peer that has seen this device
 * before rejects as a replay. Wait a bounded time for SNTP, then try anyway:
 * an installation without NTP should still be able to use the tunnel. */
#define WG_TIME_WAIT_MS     20000

static TaskHandle_t task_h;
static SemaphoreHandle_t lock;

static wg_cfg_t cfg;
static bool cfg_loaded;
static bool net_up;

static wireguard_config_t wgconf;
static wireguard_ctx_t ctx;
static bool inited;
static bool peer_connected;
static bool default_set;
static uint32_t connect_started_ms;
static uint32_t next_try_ms;
static uint32_t wait_started_ms;
static bool time_waited;

static wg_link_status_t status;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Tick wraparound safe: compare as signed difference. */
static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

static void set_state(wg_link_state_t state, const char *error)
{
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    status.state = state;
    snprintf(status.error, sizeof(status.error), "%s", error ? error : "");
    xSemaphoreGive(lock);
}

/* Tear down whatever the library currently owns.
 *
 * esp_wireguard_disconnect() dereferences ctx.netif unconditionally, and a
 * failed connect can leave a netif behind that was created but never got a
 * peer. Both cases are handled by testing ctx.netif rather than a separate
 * "connected" flag. */
static void teardown(void)
{
    if (ctx.netif) {
        esp_err_t e = esp_wireguard_disconnect(&ctx);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(e));
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    inited = false;
    peer_connected = false;
    default_set = false;
    connect_started_ms = 0;

    xSemaphoreTake(lock, portMAX_DELAY);
    status.peer_up = false;
    status.have_handshake = false;
    status.handshake_age_s = 0;
    status.endpoint_ip[0] = '\0';
    xSemaphoreGive(lock);
}

static const char *err_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK:               return "ok";
    case ESP_ERR_INVALID_ARG:  return "ungueltige Konfiguration";
    case ESP_ERR_INVALID_STATE:return "DNS nicht bereit";
    case ESP_ERR_INVALID_IP:   return "Endpunkt nicht aufloesbar";
    case ESP_ERR_RETRY:        return "DNS laeuft";
    case ESP_ERR_NO_MEM:       return "kein Speicher";
    default:                   return esp_err_to_name(e);
    }
}

static bool config_ready(char *err, size_t err_sz)
{
    ip4_addr_t a, m;

    if (!cfg.endpoint[0]) {
        snprintf(err, err_sz, "Endpunkt fehlt");
        return false;
    }
    if (!cfg.private_key[0]) {
        snprintf(err, err_sz, "privater Schluessel fehlt");
        return false;
    }
    if (!cfg.public_key[0]) {
        snprintf(err, err_sz, "oeffentlicher Schluessel der Gegenstelle fehlt");
        return false;
    }
    if (!cfg.address[0]) {
        snprintf(err, err_sz, "Tunnel-Adresse fehlt");
        return false;
    }
    if (ip4addr_aton(cfg.address, &a) != 1) {
        snprintf(err, err_sz, "Tunnel-Adresse ist keine IPv4-Adresse");
        return false;
    }
    if (ip4_addr_isany_val(a)) {
        snprintf(err, err_sz, "Tunnel-Adresse darf nicht 0.0.0.0 sein");
        return false;
    }
    if (!cfg.netmask[0]) {
        /* wg_cfg_get() fills the default, so this is only a safety net. */
        snprintf(cfg.netmask, sizeof(cfg.netmask), "255.255.255.0");
    }
    if (ip4addr_aton(cfg.netmask, &m) != 1) {
        snprintf(err, err_sz, "Netzmaske ist keine IPv4-Adresse");
        return false;
    }
    /* 0.0.0.0 would match every destination in lwIP's route lookup and shadow
     * the local LAN netif. "Route everything" is what the default-route switch
     * is for; it does the same thing without cutting off the LAN. */
    if (ip4_addr_isany_val(m)) {
        snprintf(err, err_sz,
                 "Netzmaske 0.0.0.0 nicht erlaubt - Standardroute verwenden");
        return false;
    }
    return true;
}

static bool start_link(void)
{
    memset(&wgconf, 0, sizeof(wgconf));
    /* These are borrowed, not copied, by the library: the strings have to
     * outlive the connection. cfg lives for the process lifetime. */
    wgconf.private_key = cfg.private_key;
    wgconf.public_key = cfg.public_key;
    wgconf.preshared_key = cfg.preshared_key[0] ? cfg.preshared_key : NULL;
    wgconf.address = cfg.address;
    wgconf.netmask = cfg.netmask;
    wgconf.endpoint = cfg.endpoint;
    wgconf.port = cfg.port ? cfg.port : 51820;
    wgconf.persistent_keepalive = cfg.keepalive;
    ip_addr_set_zero(&wgconf.endpoint_ip);

    esp_err_t e = esp_wireguard_init(&wgconf, &ctx);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "init: %s (%s)", err_name(e), esp_err_to_name(e));
        memset(&ctx, 0, sizeof(ctx));
        set_state(WG_LINK_ERROR, err_name(e));
        return false;
    }
    inited = true;
    return true;
}

static esp_err_t try_connect(void)
{
    esp_err_t e = esp_wireguard_connect(&ctx);
    if (e == ESP_OK) {
        /* The device's own address/32 is added by the library. The route the
         * netmask installs is narrower than this, so allowing any source is
         * the ESPHome default and keeps inbound connections through the
         * tunnel possible. */
        esp_err_t a = esp_wireguard_add_allowed_ip(&ctx, "0.0.0.0", "0.0.0.0");
        if (a != ESP_OK) {
            ESP_LOGW(TAG, "add allowed ip: %s", esp_err_to_name(a));
        }
        peer_connected = true;
        connect_started_ms = now_ms();
        set_state(WG_LINK_CONNECTING, "");
        ESP_LOGI(TAG, "peer %s:%u configured, waiting for handshake",
                 cfg.endpoint, (unsigned)wgconf.port);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "connect: %s (%s)", err_name(e), esp_err_to_name(e));
    set_state(WG_LINK_ERROR, err_name(e));
    /* A failure part-way through leaves a netif that the next init would
     * collide with (the library keeps it in a file-scope static), so clean up
     * before scheduling the next attempt. */
    teardown();
    return e;
}

static void update_peer(void)
{
    time_t handshake = 0;
    bool have = false;
    uint32_t age = 0;
    char ip[16] = "";
    bool up = esp_wireguard_peer_is_up(&ctx) == ESP_OK;
    bool was_up = status.state == WG_LINK_UP;

    if (up) {
        /* The route is installed only once the peer is really reachable.
         * Before that, making the tunnel the default would silently black-hole
         * every outgoing connection. The local LAN keeps matching the Wi-Fi
         * netif, so this does not cut off the web interface on the LAN. */
        if (cfg.default_route && !default_set) {
            /* esp_netif resets the lwIP default on every DHCP GOT_IP event
             * (lease renewal included). Marking the Wi-Fi esp_netif as a
             * deliberate default sets its internal "overridden" flag, so the
             * WireGuard default installed immediately afterwards survives
             * those events. The gateway is not used for routing here. */
            esp_netif_t *wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (!wifi) {
                wifi = esp_netif_get_default_netif();
            }
            if (wifi) {
                esp_netif_set_default_netif(wifi);
            }
            esp_err_t e = esp_wireguard_set_default(&ctx);
            if (e == ESP_OK) {
                default_set = true;
                ESP_LOGI(TAG, "tunnel is now the default route");
            } else {
                ESP_LOGW(TAG, "set_default: %s", esp_err_to_name(e));
            }
        }
        if (esp_wireguard_latest_handshake(&ctx, &handshake) == ESP_OK) {
            have = true;
            if (net_time_valid()) {
                time_t now = 0;
                time(&now);
                if (handshake > 0 && now >= handshake) {
                    age = (uint32_t)(now - handshake);
                }
            }
        }
        if (!ip_addr_isany(&wgconf.endpoint_ip)) {
            snprintf(ip, sizeof(ip), "%s", ipaddr_ntoa(&wgconf.endpoint_ip));
        }
        if (!was_up) {
            ESP_LOGI(TAG, "peer up, endpoint %s", ip[0] ? ip : "?");
            /* The broker may be reached through the tunnel. A client that
             * gave up during the outage should not wait out its own backoff,
             * and one that found another path must move back to the tunnel. */
            if (mqtt_pub_running()) {
                mqtt_pub_restart();
            }
        }
        set_state(WG_LINK_UP, "");
    } else {
        if (was_up) {
            ESP_LOGW(TAG, "peer went away");
            /* A peer that was up and then vanished gets a fresh minute before
             * the status blames the configuration. */
            connect_started_ms = now_ms();
        }
        /* Give the normal default back while the handshake is gone; otherwise
         * NTP and any LAN-to-WAN traffic would be sent into a dead tunnel. */
        if (default_set) {
            if (esp_wireguard_restore_default(&ctx) == ESP_OK) {
                default_set = false;
            }
        }
        /* "Connecting" forever is not a diagnosis. After a minute without a
         * handshake the keys or the endpoint are the likely cause, and the
         * status page should say so while the library keeps retrying. */
        if (connect_started_ms &&
            (uint32_t)(now_ms() - connect_started_ms) > 60000) {
            if (status.state != WG_LINK_ERROR) {
                ESP_LOGW(TAG, "no handshake after 60 s - keys or endpoint?");
            }
            set_state(WG_LINK_ERROR,
                      "kein Handshake - Schluessel und Endpunkt pruefen");
        } else {
            set_state(WG_LINK_CONNECTING, "");
        }
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    status.peer_up = up;
    status.have_handshake = have;
    status.handshake_age_s = age;
    snprintf(status.endpoint_ip, sizeof(status.endpoint_ip), "%s", ip);
    xSemaphoreGive(lock);
}

static void wg_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));

        if (bits & WG_NOTIFY_APPLY) {
            wg_cfg_get(&cfg);
            cfg_loaded = true;
            wait_started_ms = now_ms();
            time_waited = false;
            next_try_ms = wait_started_ms;
            teardown();
            /* The task is created on the first enable, which can be long
             * after the station got its address; ask instead of waiting for
             * the next IP event. */
            net_status_t ns;
            net_prov_status(&ns);
            net_up = ns.state == NET_STA_CONNECTED;
            xSemaphoreTake(lock, portMAX_DELAY);
            status.enabled = cfg.enabled;
            xSemaphoreGive(lock);
        }
        if (bits & WG_NOTIFY_NET_UP) {
            net_up = true;
            if (cfg_loaded && cfg.enabled) {
                next_try_ms = now_ms();
                wait_started_ms = now_ms();
                time_waited = false;
            }
        }
        if (bits & WG_NOTIFY_NET_DOWN) {
            net_up = false;
        }

        if (!cfg_loaded) {
            continue;
        }

        uint32_t now = now_ms();

        if (!cfg.enabled) {
            if (inited) {
                teardown();
            }
            set_state(WG_LINK_DISABLED, "");
            continue;
        }

        if (!net_up) {
            if (inited) {
                teardown();
            }
            set_state(WG_LINK_WAITING, "kein Netz");
            continue;
        }

        if (!net_time_valid() && !time_waited) {
            if ((uint32_t)(now - wait_started_ms) >= WG_TIME_WAIT_MS) {
                time_waited = true;
                ESP_LOGW(TAG, "clock still not synchronised after %us - "
                         "starting the tunnel anyway",
                         WG_TIME_WAIT_MS / 1000);
            } else {
                set_state(WG_LINK_WAITING, "wartet auf Zeitsynchronisation");
                continue;
            }
        }

        if (!inited) {
            if (!due(now, next_try_ms)) {
                set_state(WG_LINK_WAITING, "wartet auf naechsten Versuch");
                continue;
            }
            char err[96] = "";
            if (!config_ready(err, sizeof(err))) {
                set_state(WG_LINK_ERROR, err);
                next_try_ms = now + WG_ERROR_RETRY_MS;
                continue;
            }
            if (!start_link()) {
                next_try_ms = now + WG_ERROR_RETRY_MS;
                continue;
            }
            next_try_ms = now;
        }

        if (!peer_connected) {
            if (!due(now, next_try_ms)) {
                continue;
            }
            esp_err_t e = try_connect();
            if (e == ESP_OK) {
                next_try_ms = now;
            } else if (e == ESP_ERR_RETRY) {
                next_try_ms = now + WG_DNS_RETRY_MS;
            } else {
                next_try_ms = now + WG_ERROR_RETRY_MS;
            }
            continue;
        }

        update_peer();
    }
}

void wg_link_init(void)
{
    if (lock || task_h) {
        return;
    }
    /* Do NOT call sodium_init() here.
     *
     * The esphome/libsodium build used by the WireGuard component has no
     * ESP-IDF sysrandom backend; sodium_init() ends in randombytes_sysrandom
     * and aborts in sodium_misuse(). The library does not need it either:
     * WireGuard's randomness comes from mbedTLS/PSA, and the only libsodium
     * functions used here are the pure curve25519 and base64 helpers. */
    lock = xSemaphoreCreateMutex();
    if (!lock) {
        ESP_LOGE(TAG, "no mutex");
        return;
    }
    memset(&status, 0, sizeof(status));
    status.state = WG_LINK_DISABLED;

    /* Same path as a settings change, so boot and save use one code path. */
    wg_link_restart();
}

void wg_link_network_up(void)
{
    if (task_h) {
        xTaskNotify(task_h, WG_NOTIFY_NET_UP, eSetBits);
    }
}

void wg_link_network_down(void)
{
    if (task_h) {
        xTaskNotify(task_h, WG_NOTIFY_NET_DOWN, eSetBits);
    }
}

/* The task exists only once WireGuard has been enabled. Most installations
 * never do, and its 8 KB stack would otherwise come out of the internal RAM
 * that the codec trees and the Wi-Fi stack already compete for. Once created
 * it stays: a later disable parks it on its notification instead of deleting
 * it, which avoids racing a restart against the deletion. */
void wg_link_restart(void)
{
    if (!lock) {
        return;
    }
    wg_cfg_t c;
    wg_cfg_get(&c);
    xSemaphoreTake(lock, portMAX_DELAY);
    status.enabled = c.enabled;
    if (!task_h && c.enabled) {
        if (xTaskCreate(wg_task, "wireguard", 8192, NULL, 5, &task_h) != pdPASS) {
            ESP_LOGE(TAG, "could not create the tunnel task");
            task_h = NULL;
            status.state = WG_LINK_ERROR;
            snprintf(status.error, sizeof(status.error), "kein Speicher fuer den Tunnel-Task");
        }
    }
    xSemaphoreGive(lock);
    if (task_h) {
        xTaskNotify(task_h, WG_NOTIFY_APPLY, eSetBits);
    }
}

void wg_link_status(wg_link_status_t *out)
{
    if (!out) {
        return;
    }
    if (!lock) {
        memset(out, 0, sizeof(*out));
        out->state = WG_LINK_DISABLED;
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(lock);
}

bool wg_link_device_public_key(char *out, size_t out_sz)
{
    if (!out || out_sz < CFG_WG_KEY_MAX) {
        return false;
    }
    out[0] = '\0';

    wg_cfg_t c;
    wg_cfg_get(&c);
    if (!c.private_key[0]) {
        return false;
    }

    /* WireGuard private keys are base64 of 32 bytes. libsodium's scalarmult
     * applies the same clamping wg(8) does, so this matches `wg pubkey`. */
    unsigned char priv[32];
    unsigned char pub[32];
    size_t len = 0;
    if (sodium_base642bin(priv, sizeof(priv), c.private_key,
                          strlen(c.private_key), NULL, &len, NULL,
                          sodium_base64_VARIANT_ORIGINAL) != 0 ||
        len != sizeof(priv)) {
        return false;
    }
    if (crypto_scalarmult_curve25519_base(pub, priv) != 0) {
        return false;
    }
    if (!sodium_bin2base64(out, out_sz, pub, sizeof(pub),
                           sodium_base64_VARIANT_ORIGINAL)) {
        out[0] = '\0';
        return false;
    }
    return true;
}
