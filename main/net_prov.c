#include "net_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"

#include "app_config.h"
#include "o3e_json.h"
#include "wg_link.h"

static const char *TAG = "net";

static void start_time_sync(void);

/* wifi_config_t carries SSIDs and PSKs in fixed-size, length-delimited fields
 * rather than as C strings, so snprintf both warns about truncation and wastes
 * the last byte. Copying explicitly says what is actually meant. */
static void copy_field(uint8_t *dst, size_t dst_sz, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_sz) {
        n = dst_sz;
    }
    memcpy(dst, src, n);
    if (n < dst_sz) {
        memset(dst + n, 0, dst_sz - n);
    }
}

static net_status_t status;
static esp_netif_t *netif_sta;
static esp_netif_t *netif_ap;
static bool setup_mode;
/* The station interface exists in setup mode too (scanning needs it) but must
 * not try to associate with anything until credentials are stored. */
static bool sta_should_connect;

/* ------------------------------------------------------------------ */
/* Captive portal DNS                                                   */

/* Phones only pop up the sign-in page if every name resolves to us, so the
 * responder answers each A query with the soft-AP address. It deliberately
 * understands just enough DNS to do that and nothing else. */
#define DNS_PORT 53
#define AP_IP    "192.168.4.1"

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (setup_mode) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *h = (dns_header_t *)buf;
        if (ntohs(h->qd_count) != 1) {
            continue;
        }

        /* Walk the single question to find where the answer should start. */
        uint8_t *p = buf + sizeof(dns_header_t);
        uint8_t *end = buf + n;
        while (p < end && *p) {
            p += *p + 1;
        }
        p += 1 + 4;   /* the root label, then QTYPE and QCLASS */
        if (p > end || (size_t)(p - buf) + 16 > sizeof(buf)) {
            continue;
        }

        h->flags = htons(0x8180);   /* response, recursion available */
        h->an_count = htons(1);
        h->ns_count = 0;
        h->ar_count = 0;

        /* Answer: pointer back to the question name, A record, our address. */
        *p++ = 0xC0;
        *p++ = 0x0C;
        *p++ = 0x00; *p++ = 0x01;             /* TYPE A */
        *p++ = 0x00; *p++ = 0x01;             /* CLASS IN */
        *p++ = 0x00; *p++ = 0x00;
        *p++ = 0x00; *p++ = 0x3C;             /* TTL 60 s */
        *p++ = 0x00; *p++ = 0x04;             /* RDLENGTH */
        uint32_t ip = inet_addr(AP_IP);
        memcpy(p, &ip, 4);
        p += 4;

        sendto(sock, buf, p - buf, 0, (struct sockaddr *)&src, slen);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

static void start_ap(const wifi_cfg_t *cfg, bool alongside_sta);

/* Called once the stored network has stayed unreachable for NET_STA_TIMEOUT_MS.
 *
 * The device switches to AP+STA rather than to AP alone, and keeps retrying in
 * the background.  A router that is merely rebooting will be joined a minute
 * later without anyone noticing, while a genuinely wrong password still leaves
 * a reachable setup page.  Dropping to AP-only, or rebooting, would turn every
 * brief outage into a manual recovery. */
static void fallback_to_setup_ap(void)
{
    if (setup_mode) {
        return;
    }
    ESP_LOGW(TAG, "'%s' unreachable for %us - raising the setup AP alongside",
             status.ssid, NET_STA_TIMEOUT_MS / 1000);
    wifi_cfg_t cfg;
    wifi_cfg_get(&cfg);
    start_ap(&cfg, true);
}

static void sta_timeout_cb(void *arg)
{
    (void)arg;
    if (status.state != NET_STA_CONNECTED) {
        fallback_to_setup_ap();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (sta_should_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!sta_should_connect) {
            return;   /* setup mode: the station interface is only for scanning */
        }
        status.state = NET_STA_CONNECTING;
        status.ip[0] = '\0';
        status.retries++;
        /* The tunnel's endpoint is no longer reachable; the task parks until
         * the next address arrives instead of hammering the handshake. */
        wg_link_network_down();
        /* Keep retrying indefinitely; the timeout above decides when the setup
         * AP is needed. Backing off a little avoids hammering a busy router. */
        vTaskDelay(pdMS_TO_TICKS(status.retries > NET_STA_RETRIES ? 5000 : 1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(status.ip, sizeof(status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        status.state = NET_STA_CONNECTED;
        status.retries = 0;
        ESP_LOGI(TAG, "connected, ip %s", status.ip);
        start_time_sync();
        /* WireGuard waits for this: its handshake timestamps come from the
         * wall clock, and SNTP is what sets it. */
        wg_link_network_up();
    }
}

/* Set the clock from the network.
 *
 * Without this the device counts microseconds since boot and nothing more,
 * which is enough to order events against each other but not to line them up
 * with anything outside: a trace showing a control datapoint changing at
 * "t=3891.4 s" cannot be matched against a heating app that says grid charging
 * began at 14:32. The timezone comes from the settings so log lines and the
 * web interface read in local time.
 *
 * Started once and left running; it re-syncs by itself and survives the
 * reconnects that this handler otherwise sees. */
static void start_time_sync(void)
{
    static bool started;
    if (started) {
        return;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (sys.tz[0]) {
        setenv("TZ", sys.tz, 1);
        tzset();
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        started = true;
        ESP_LOGI(TAG, "time sync started (TZ=%s)", sys.tz);
    }
}

bool net_time_valid(void)
{
    time_t now = 0;
    time(&now);
    /* Anything before 2023 is the epoch the clock starts at, not a real date. */
    return now > 1672531200;
}

static void start_mdns(const char *hostname)
{
    if (mdns_init() != ESP_OK) {
        return;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set("open3e CAN gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", hostname);
}

/* `alongside_sta` distinguishes the fallback case -- a station connection that
 * is already running and should keep retrying -- from a first boot. Either way
 * the radio ends up in AP+STA: the ESP32 cannot scan for networks with only
 * the AP interface up, and the setup page is useless without a network list. */
static void start_ap(const wifi_cfg_t *cfg, bool alongside_sta)
{
    setup_mode = true;
    if (!alongside_sta) {
        status.state = NET_AP_SETUP;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(status.ap_ssid, sizeof(status.ap_ssid), "Open3E-Setup-%02X%02X", mac[4], mac[5]);

    if (!netif_ap) {
        netif_ap = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t wc = { 0 };
    copy_field(wc.ap.ssid, sizeof(wc.ap.ssid), status.ap_ssid);
    wc.ap.ssid_len = strlen(status.ap_ssid);
    wc.ap.max_connection = 4;
    wc.ap.channel = 1;

    const char *pass = cfg->ap_pass[0] ? cfg->ap_pass : "open3e-setup";
    if (strlen(pass) >= 8) {
        copy_field(wc.ap.password, sizeof(wc.ap.password), pass);
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        /* WPA2 needs eight characters; rather than silently opening the
         * network, fall back to the documented default. */
        copy_field(wc.ap.password, sizeof(wc.ap.password), "open3e-setup");
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    if (!alongside_sta) {
        /* Create the station netif as well so esp_wifi_scan_start() has an
         * interface to work with; sta_should_connect keeps it from associating. */
        if (!netif_sta) {
            netif_sta = esp_netif_create_default_wifi_sta();
        }
        ESP_ERROR_CHECK(esp_wifi_start());
        snprintf(status.ip, sizeof(status.ip), AP_IP);
    }
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "setup AP '%s' up, open http://%s", status.ap_ssid, AP_IP);
}

static void start_sta(const wifi_cfg_t *cfg)
{
    setup_mode = false;
    sta_should_connect = true;
    status.state = NET_STA_CONNECTING;
    snprintf(status.ssid, sizeof(status.ssid), "%s", cfg->ssid);

    if (!netif_sta) {
        netif_sta = esp_netif_create_default_wifi_sta();
    }

    wifi_config_t wc = { 0 };
    copy_field(wc.sta.ssid, sizeof(wc.sta.ssid), cfg->ssid);
    copy_field(wc.sta.password, sizeof(wc.sta.password), cfg->pass);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns(cfg->hostname[0] ? cfg->hostname : "open3e");

    /* One-shot: if no address has arrived by then, make the device reachable
     * again through its own AP. */
    const esp_timer_create_args_t ta = {
        .callback = sta_timeout_cb, .name = "sta_timeout",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)NET_STA_TIMEOUT_MS * 1000);
    }
}

void net_prov_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_cfg_t cfg;
    wifi_cfg_get(&cfg);
    if (cfg.ssid[0]) {
        start_sta(&cfg);
    } else {
        start_ap(&cfg, false);
    }
}

void net_prov_status(net_status_t *out)
{
    *out = status;
    if (status.state == NET_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

bool net_prov_is_setup_mode(void) { return setup_mode; }

char *net_prov_scan_json(void)
{
    o3e_buf_t out;
    o3e_buf_init(&out);

    /* In AP mode a scan briefly interrupts the soft AP, so it is done on
     * demand from the setup page rather than continuously. */
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        /* Returning an empty list here would be indistinguishable from "there
         * really are no networks", which is exactly the wrong thing to tell
         * someone standing in a basement wondering why setup will not proceed. */
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        o3e_buf_adds(&out, "{\"error\": ");
        o3e_buf_add_json_str(&out, esp_err_to_name(err));
        o3e_buf_addc(&out, '}');
        return out.buf ? out.buf : strdup("{\"error\": \"scan failed\"}");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) {
        n = 40;
    }
    wifi_ap_record_t *recs = n ? calloc(n, sizeof(*recs)) : NULL;
    if (n && !recs) {
        esp_wifi_clear_ap_list();
        return strdup("{\"error\": \"out of memory\"}");
    }
    if (n) {
        esp_wifi_scan_get_ap_records(&n, recs);
    } else {
        esp_wifi_clear_ap_list();
    }

    ESP_LOGI(TAG, "scan found %u access point(s)", n);

    o3e_buf_addc(&out, '[');
    uint16_t emitted = 0;
    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) {
            continue;   /* hidden network: nothing for the user to pick */
        }
        /* A mesh or a dual-band router shows up several times; the dropdown
         * should list each name once, keeping the strongest reading. */
        bool dup = false;
        for (uint16_t k = 0; k < i; k++) {
            if (strcmp((const char *)recs[k].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        char t[48];
        if (emitted++) {
            o3e_buf_adds(&out, ", ");
        }
        o3e_buf_adds(&out, "{\"ssid\": ");
        o3e_buf_add_json_str(&out, ssid);
        snprintf(t, sizeof(t), ", \"rssi\": %d, \"secure\": %s}",
                 recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        o3e_buf_adds(&out, t);
    }
    o3e_buf_addc(&out, ']');

    free(recs);
    return out.buf ? out.buf : strdup("[]");
}
