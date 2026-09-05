#include "can_port.h"

#include <string.h>

#include "cantrace.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "can";

/* ESP-IDF 6.x node API (esp_twai.h). Receiving is interrupt-driven there:
 * frames are handed to us inside an ISR callback, so they are copied into a
 * queue that the blocking hal_recv() below drains. That is a better fit for
 * ISO-TP than the old polling driver anyway -- inter-frame timing stops
 * depending on how often a task happens to look at the peripheral.
 *
 * All peripheral contact is confined to this file. */

/* One CAN frame as it travels from the receive ISR to the ISO-TP layer. */
typedef struct {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[8];
} rx_frame_t;

/* Exclusive access to the bus.
 *
 * open3e warns "do not start more than one instance"; here the competing users
 * are internal (poller, scanner, web UI, MQTT commands). A mutex held for the
 * duration of one UDS exchange gives exactly the required property: at most one
 * ISO-TP session at a time, driven by whichever task asked for it.
 *
 * An earlier version handed requests to a dedicated task through a queue. That
 * was a mistake: xQueueSend copies the request by value, so the task filled in
 * the result on its own copy while every caller read back the zero-initialised
 * result from its own stack -- which decodes as UDS_OK. Every request appeared
 * to succeed, and the scan "found" a device at every address it probed. */
/* Diagnostics tap. When armed, the ISR only counts -- nothing is queued and
 * nothing is decoded, so a sweep cannot disturb an exchange. */
static volatile bool diag_armed;
static can_diag_t    diag_acc;

/* Passive listeners, consulted from the receive ISR. */
typedef struct {
    volatile can_listen_cb_t cb;
    volatile uint32_t        first;
    volatile uint32_t        last;
    /* n_ids == 0 (the default, set by can_port_add_listener()) preserves the
     * original behavior: every frame in [first, last] goes to cb. When
     * can_port_add_id_listener() set n_ids > 0, only frames whose ID is
     * actually in ids[] do - anything else in [first, last] falls through to
     * the ISO-TP queue below instead. */
    uint16_t                 ids[CAN_LISTENER_MAX_IDS];
    volatile uint8_t         n_ids;
} listener_t;

static listener_t listeners[CAN_MAX_LISTENERS];

static SemaphoreHandle_t  bus_lock;
/* Written only by the task holding the lock, read by the status endpoint. */
static char               bus_holder[16];
static uint32_t           bus_taken_ms;
static QueueHandle_t      rx_q;
static twai_node_handle_t node;
static volatile bool      running;
static can_stats_t        stats;

/* ------------------------------------------------------------------ */
/* ISO-TP HAL backed by TWAI                                            */

static bool hal_send(void *ctx, uint32_t id, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    /* The driver's transmit queue stores the frame by pointer, not by value
     * (twai_frame_queue.c: `.data = data`), so this buffer has to stay valid
     * until the frame has actually gone out. That is what the wait below is
     * for -- it looks removable and is not. */
    uint8_t payload[8];
    memcpy(payload, data, len);

    twai_frame_t frame = {
        .header = { .id = id, .dlc = len },
        .buffer = payload,
        .buffer_len = len,
    };
    /* Recorded before the result is known: a frame that failed to go out is
     * still part of the story when reading a trace back. */
    cantrace_put((uint16_t)id, payload, len, true);

    if (twai_node_transmit(node, &frame, 50) != ESP_OK) {
        stats.tx_failed++;
        return false;
    }
    if (twai_node_transmit_wait_all_done(node, 100) != ESP_OK) {
        stats.tx_failed++;
        return false;
    }
    return true;
}

static bool hal_recv(void *ctx, uint32_t *id, uint8_t *data, uint8_t *len,
                     uint32_t timeout_ms)
{
    (void)ctx;
    rx_frame_t f;
    if (xQueueReceive(rx_q, &f, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    *id = f.id;
    *len = f.len;
    memcpy(data, f.data, f.len);
    return true;
}

/* Runs in interrupt context: copy the frame out and let the waiting task deal
 * with it. Extended-ID and RTR frames are dropped here -- the E3 protocol uses
 * neither, and filtering early keeps them out of the ISO-TP state machine. */
static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle,
                                 const twai_rx_done_event_data_t *edata,
                                 void *user_ctx)
{
    (void)edata;
    (void)user_ctx;
    uint8_t buf[8];
    twai_frame_t frame = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }
    if (frame.header.ide || frame.header.rtr) {
        return false;
    }

    uint8_t dlc = (uint8_t)frame.header.dlc;
    if (dlc > sizeof(buf)) {
        dlc = sizeof(buf);
    }

    /* Ahead of any routing, so the trace sees the whole bus rather than only
     * what this gateway happens to be interested in. */
    cantrace_put((uint16_t)frame.header.id, buf, dlc, false);

    if (diag_armed) {
        diag_acc.frames++;
        bool known = false;
        for (uint8_t i = 0; i < diag_acc.n_ids; i++) {
            if (diag_acc.ids[i] == frame.header.id) {
                known = true;
                break;
            }
        }
        if (!known && diag_acc.n_ids < CAN_DIAG_MAX_IDS) {
            diag_acc.ids[diag_acc.n_ids++] = frame.header.id;
        }
        return false;
    }

    /* Broadcast frames are handed straight to every listener that wants them
     * rather than to the ISO-TP queue - putting them there instead would both
     * confuse an exchange in progress and lose them to the next flush_rx().
     *
     * Every matching listener is called, not just the first: a raw relay
     * (see raw_relay.c) may be asked for the very same IDs em380.c or
     * collect.c already own, because it exists precisely to hand a caller
     * the bytes those already decode for this firmware's own use. Nothing
     * before this had two listeners wanting the same ID at once, so this
     * used to return on the first match; that was never a promise "only one
     * listener will see this frame", just what happened to always be true. */
    bool matched = false;
    BaseType_t woken = pdFALSE;
    for (int i = 0; i < CAN_MAX_LISTENERS; i++) {
        can_listen_cb_t cb = listeners[i].cb;
        if (!cb || frame.header.id < listeners[i].first ||
            frame.header.id > listeners[i].last) {
            continue;
        }
        if (listeners[i].n_ids > 0) {
            bool in_ids = false;
            for (uint8_t k = 0; k < listeners[i].n_ids; k++) {
                if (listeners[i].ids[k] == frame.header.id) {
                    in_ids = true;
                    break;
                }
            }
            if (!in_ids) {
                continue;
            }
        }
        matched = true;
        if (cb(frame.header.id, buf, dlc)) {
            woken = pdTRUE;
        }
    }
    if (matched) {
        return woken == pdTRUE;
    }

    rx_frame_t f = { .id = frame.header.id, .len = dlc };
    memcpy(f.data, buf, f.len);

    BaseType_t rx_woken = pdFALSE;
    if (xQueueSendFromISR(rx_q, &f, &rx_woken) != pdTRUE) {
        stats.rx_missed++;
    }
    return rx_woken == pdTRUE;
}

/* A wiring fault or a wrong bitrate drives the controller bus-off. Recovery
 * has to be asked for explicitly; without it the gateway goes quiet with no
 * visible cause. */
static bool IRAM_ATTR on_state_change(twai_node_handle_t handle,
                                      const twai_state_change_event_data_t *edata,
                                      void *user_ctx)
{
    (void)user_ctx;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        stats.recovery_count++;
        twai_node_recover(handle);
    }
    return false;
}

static void hal_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    /* Below a tick, busy-wait: vTaskDelay would round a 100 us STmin up to a
     * whole tick and slow segmented transfers to a crawl. */
    if (us < 1000 * portTICK_PERIOD_MS) {
        esp_rom_delay_us(us);
    } else {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
    }
}

static uint32_t hal_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const isotp_hal_t twai_hal = {
    .send = hal_send, .recv = hal_recv,
    .delay_us = hal_delay_us, .now_ms = hal_now_ms, .ctx = NULL,
};

/* ------------------------------------------------------------------ */

static void update_stats(void)
{
    twai_node_status_t st;
    twai_node_record_t rec;
    if (!node || twai_node_get_info(node, &st, &rec) != ESP_OK) {
        return;
    }
    stats.bus_errors = rec.bus_err_num;
    stats.tx_err_count = st.tx_error_count;
    stats.rx_err_count = st.rx_error_count;
    switch (st.state) {
    case TWAI_ERROR_ACTIVE:  stats.state = "running";       break;
    case TWAI_ERROR_WARNING: stats.state = "error-warning"; break;
    case TWAI_ERROR_PASSIVE: stats.state = "error-passive"; break;
    case TWAI_ERROR_BUS_OFF: stats.state = "bus-off";       break;
    default:                 stats.state = "unknown";       break;
    }
}

/* Drain stale frames so a fresh request never reads a previous session's
 * leftovers -- a timed-out request can leave consecutive frames queued. */
static void flush_rx(void)
{
    xQueueReset(rx_q);
}

/* All node creation goes through here so the diagnostics differ from normal
 * operation only in bit rate and flags. */
static bool node_create(uint32_t bitrate, bool loopback, bool self_test,
                        bool listen_only)
{
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = CAN_TX_GPIO,
            .rx = CAN_RX_GPIO,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = { .bitrate = bitrate },
        /* No hardware retransmission: an unacknowledged frame is a fact worth
         * reporting to the ISO-TP layer, not something to repeat silently. The
         * field is zero-initialised to the same value; saying it explicitly
         * keeps it from looking like an oversight. */
        .fail_retry_cnt = 0,
        .tx_queue_depth = 8,
        .intr_priority = 0,
        .flags = {
            .enable_loopback = loopback,
            .enable_self_test = self_test,
            .enable_listen_only = listen_only,
        },
    };
    if (twai_new_node_onchip(&cfg, &node) != ESP_OK) {
        node = NULL;
        return false;
    }
    const twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
    };
    if (twai_node_register_event_callbacks(node, &cbs, NULL) != ESP_OK ||
        twai_node_enable(node) != ESP_OK) {
        twai_node_delete(node);
        node = NULL;
        return false;
    }
    return true;
}

static void node_destroy(void)
{
    if (node) {
        twai_node_disable(node);
        twai_node_delete(node);
        node = NULL;
    }
}

bool can_port_start(void)
{
    if (running) {
        return true;
    }

    /* A 4 KiB ISO-TP message is around 585 frames back to back, but the peer
     * gates them with STmin; 64 is ample and bounds the ISR-side buffer. */
    rx_q = xQueueCreate(64, sizeof(rx_frame_t));
    bus_lock = xSemaphoreCreateMutex();
    if (!rx_q || !bus_lock) {
        goto fail;
    }

    if (!node_create(CAN_BITRATE, false, false, false)) {
        ESP_LOGE(TAG, "could not bring up the TWAI node");
        goto fail;
    }

    running = true;
    stats.running = true;
    ESP_LOGI(TAG, "TWAI up: %d kbit/s, TX=GPIO%d RX=GPIO%d",
             CAN_BITRATE / 1000, CAN_TX_GPIO, CAN_RX_GPIO);
    return true;

fail:
    node_destroy();
    if (rx_q) {
        vQueueDelete(rx_q);
        rx_q = NULL;
    }
    if (bus_lock) {
        vSemaphoreDelete(bus_lock);
        bus_lock = NULL;
    }
    return false;
}

void can_port_stop(void)
{
    if (!running) {
        return;
    }
    running = false;
    stats.running = false;
    /* Wait out any exchange in flight so the peripheral is not torn down
     * underneath it. */
    if (xSemaphoreTake(bus_lock, pdMS_TO_TICKS(30000)) == pdTRUE) {
        xSemaphoreGive(bus_lock);
    }
    node_destroy();
    vSemaphoreDelete(bus_lock);
    bus_lock = NULL;
    vQueueDelete(rx_q);
    rx_q = NULL;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                          */

/* Both tests replace the node, so they take the bus for their duration and
 * put normal operation back afterwards even on failure. */
static bool diag_take_bus(char *err, size_t err_sz)
{
    if (!running || !bus_lock) {
        snprintf(err, err_sz, "the CAN interface is not running");
        return false;
    }
    if (xSemaphoreTake(bus_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        snprintf(err, err_sz, "the bus is busy; stop the scan first");
        return false;
    }
    snprintf(bus_holder, sizeof(bus_holder), "diag");
    bus_taken_ms = hal_now_ms(NULL);
    node_destroy();
    return true;
}

static void diag_release_bus(void)
{
    diag_armed = false;
    bus_holder[0] = '\0';
    if (!node) {
        node_create(CAN_BITRATE, false, false, false);
    }
    xSemaphoreGive(bus_lock);
}

bool can_diag_loopback(char *err, size_t err_sz)
{
    if (!diag_take_bus(err, err_sz)) {
        return false;
    }

    bool ok = false;
    /* Loopback plus self-test: the controller hears its own frame and does not
     * need anybody to acknowledge it. */
    if (!node_create(CAN_BITRATE, true, true, false)) {
        snprintf(err, err_sz, "could not open the controller in loopback mode");
        diag_release_bus();
        return false;
    }

    xQueueReset(rx_q);
    uint8_t payload[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    twai_frame_t tx = {
        .header = { .id = 0x7FF, .dlc = sizeof(payload) },
        .buffer = payload,
        .buffer_len = sizeof(payload),
    };
    if (twai_node_transmit(node, &tx, 100) == ESP_OK) {
        rx_frame_t got;
        if (xQueueReceive(rx_q, &got, pdMS_TO_TICKS(200)) == pdTRUE) {
            ok = got.id == 0x7FF && got.len == sizeof(payload) &&
                 memcmp(got.data, payload, sizeof(payload)) == 0;
        }
    }
    if (!ok) {
        snprintf(err, err_sz,
                 "the controller did not receive its own frame - this is a "
                 "firmware or pin configuration problem, not the wiring");
    }

    node_destroy();
    diag_release_bus();
    return ok;
}

bool can_diag_listen(const uint32_t *bitrates, size_t n, uint32_t ms_each,
                     can_diag_t *out, char *err, size_t err_sz)
{
    if (!diag_take_bus(err, err_sz)) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        memset(&diag_acc, 0, sizeof(diag_acc));
        diag_acc.bitrate = bitrates[i];

        /* Listen-only never transmits and never acknowledges, so a wrong bit
         * rate here cannot disturb a healthy bus. */
        if (!node_create(bitrates[i], false, false, true)) {
            ESP_LOGW(TAG, "cannot listen at %u bit/s", (unsigned)bitrates[i]);
            out[i] = diag_acc;
            continue;
        }
        diag_armed = true;
        vTaskDelay(pdMS_TO_TICKS(ms_each));
        diag_armed = false;

        out[i] = diag_acc;
        ESP_LOGI(TAG, "listen-only %u bit/s: %u frame(s), %u distinct ID(s)",
                 (unsigned)bitrates[i], (unsigned)diag_acc.frames, diag_acc.n_ids);
        node_destroy();
    }

    diag_release_bus();
    return true;
}

bool can_port_add_listener(uint32_t first, uint32_t last, can_listen_cb_t cb)
{
    if (!cb) {
        return false;
    }
    for (int i = 0; i < CAN_MAX_LISTENERS; i++) {
        if (listeners[i].cb == cb || !listeners[i].cb) {
            /* Ordered so the ISR never observes a live callback paired with a
             * stale range: the slot is disarmed, updated, then armed. */
            listeners[i].cb = NULL;
            listeners[i].first = first;
            listeners[i].last = last;
            listeners[i].n_ids = 0;
            listeners[i].cb = cb;
            return true;
        }
    }
    ESP_LOGW(TAG, "no free listener slot");
    return false;
}

bool can_port_add_id_listener(const uint16_t *ids, size_t n_ids, can_listen_cb_t cb)
{
    if (!cb || n_ids == 0 || n_ids > CAN_LISTENER_MAX_IDS) {
        return false;
    }
    uint32_t lo = ids[0], hi = ids[0];
    for (size_t i = 1; i < n_ids; i++) {
        if (ids[i] < lo) {
            lo = ids[i];
        }
        if (ids[i] > hi) {
            hi = ids[i];
        }
    }
    for (int i = 0; i < CAN_MAX_LISTENERS; i++) {
        if (listeners[i].cb == cb || !listeners[i].cb) {
            /* Same disarm-update-arm order as can_port_add_listener(): the
             * ISR must never see n_ids/ids paired with a stale cb or range. */
            listeners[i].cb = NULL;
            listeners[i].first = lo;
            listeners[i].last = hi;
            for (size_t k = 0; k < n_ids; k++) {
                listeners[i].ids[k] = ids[k];
            }
            listeners[i].n_ids = (uint8_t)n_ids;
            listeners[i].cb = cb;
            return true;
        }
    }
    ESP_LOGW(TAG, "no free listener slot");
    return false;
}

void can_port_remove_listener(can_listen_cb_t cb)
{
    for (int i = 0; i < CAN_MAX_LISTENERS; i++) {
        if (listeners[i].cb == cb) {
            listeners[i].cb = NULL;
            listeners[i].first = 1;
            listeners[i].last = 0;
            listeners[i].n_ids = 0;
        }
    }
}

void can_port_stats(can_stats_t *out)
{
    /* Refresh only when the bus is idle. Reading the controller's counters
     * from the HTTP task while an exchange is in flight would race with the
     * task that owns it, and the numbers are not worth that. */
    if (running && bus_lock && xSemaphoreTake(bus_lock, 0) == pdTRUE) {
        update_stats();
        xSemaphoreGive(bus_lock);
    }
    *out = stats;
    snprintf(out->holder, sizeof(out->holder), "%s", bus_holder);
    out->held_ms = bus_holder[0] ? (hal_now_ms(NULL) - bus_taken_ms) : 0;
    if (!out->state) {
        out->state = "stopped";
    }
}

/* Runs the exchange in the calling task while holding the bus. The result is
 * returned directly, so there is no copy of it anywhere to get out of step. */
static uds_result_t with_bus(uint16_t ecu_tx, bool is_write, uint16_t did,
                             const uint8_t *wr, size_t wr_len,
                             uint8_t *rd, size_t rd_sz, size_t *rd_len,
                             uint32_t p2_ms)
{
    if (!running || !bus_lock) {
        return (uds_result_t){ UDS_ERR_TRANSPORT, 0 };
    }
    /* Two-stage wait. The total is comfortably longer than the worst-case
     * single exchange (P2 plus a bounded number of P2* extensions) so queueing
     * behind one in-flight request never fails spuriously -- but a holder that
     * is genuinely stuck is named after five seconds rather than thirty, which
     * is the difference between a usable log and a staring contest. */
    if (xSemaphoreTake(bus_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "waiting on bus: held by '%s' for %u s "
                 "(0x%03X.%u wants it)",
                 bus_holder[0] ? bus_holder : "?",
                 (unsigned)((hal_now_ms(NULL) - bus_taken_ms) / 1000), ecu_tx, did);
        if (xSemaphoreTake(bus_lock, pdMS_TO_TICKS(25000)) != pdTRUE) {
            ESP_LOGE(TAG, "bus still held by '%s' after 30 s; "
                     "request for 0x%03X.%u dropped",
                     bus_holder[0] ? bus_holder : "?", ecu_tx, did);
            return (uds_result_t){ UDS_ERR_TRANSPORT, 0 };
        }
    }
    snprintf(bus_holder, sizeof(bus_holder), "%s", pcTaskGetName(NULL));
    bus_taken_ms = hal_now_ms(NULL);

    /* Drain stale frames so this exchange never reads a previous session's
     * leftovers; a timed-out request can leave consecutive frames queued. */
    flush_rx();

    /* Tell the tracer this traffic is ours, so it can be excluded. */
    cantrace_own_begin(ecu_tx);

    const isotp_link_t link = {
        .hal = &twai_hal,
        .tx_id = ecu_tx,
        .rx_id = UDS_RX_OF(ecu_tx),
        .stmin_ms = ISOTP_STMIN_MS,
    };
    uint32_t started = hal_now_ms(NULL);
    uds_result_t r = is_write
        ? uds_write_did(&link, did, wr, wr_len, p2_ms)
        : uds_read_did(&link, did, rd, rd_sz, rd_len, p2_ms);

    /* One exchange is bounded to a few seconds by P2 and the pending budget.
     * Anything beyond that is the thing that stalls a scan while the bus keeps
     * transmitting, so it names itself instead of being inferred. */
    uint32_t took = hal_now_ms(NULL) - started;
    if (took > 2000) {
        ESP_LOGW(TAG, "0x%03X.%u took %u ms (%s)", ecu_tx, did,
                 (unsigned)took, uds_strerror(r));
    }

    cantrace_own_end();
    update_stats();
    bus_holder[0] = '\0';
    xSemaphoreGive(bus_lock);
    return r;
}

uds_result_t can_read_did(uint16_t ecu_tx, uint16_t did,
                          uint8_t *buf, size_t buf_sz, size_t *out_len,
                          uint32_t p2_ms)
{
    *out_len = 0;
    return with_bus(ecu_tx, false, did, NULL, 0, buf, buf_sz, out_len,
                    p2_ms ? p2_ms : UDS_P2_MS);
}

uds_result_t can_write_did(uint16_t ecu_tx, uint16_t did,
                           const uint8_t *data, size_t len, uint32_t p2_ms)
{
    size_t unused = 0;
    return with_bus(ecu_tx, true, did, data, len, NULL, 0, &unused,
                    p2_ms ? p2_ms : UDS_P2_MS);
}
