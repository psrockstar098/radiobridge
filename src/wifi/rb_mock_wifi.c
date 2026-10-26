/*
 * RadioBridge — rb_mock_wifi.c (Wi-Fi half of the mock backend)
 *
 * Deterministic fake Wi-Fi: fixed network catalog, a small state
 * machine, and simulated quality numbers derived from signal strength.
 * The simulation is documented everywhere it appears — mock quality
 * values are NEVER presented as measurements.
 *
 * Fake BSSIDs use the locally-administered pattern 02:00:00:00:0N:00.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "core/rb_mock_state.h"

static const rb_wifi_network_t g_networks[] = {
    {
        .ssid = "RB-HomeNet",
        .bssid = "02:00:00:00:01:00",
        .signal_dbm = -45,
        .frequency_mhz = 5180,
        .channel = 36,
        .security = RB_WIFI_SEC_WPA2,
    },
    {
        .ssid = "RB-CoffeeShop",
        .bssid = "02:00:00:00:02:00",
        .signal_dbm = -62,
        .frequency_mhz = 2412,
        .channel = 1,
        .security = RB_WIFI_SEC_OPEN,
    },
    {
        .ssid = "RB-Lab-5G",
        .bssid = "02:00:00:00:03:00",
        .signal_dbm = -58,
        .frequency_mhz = 5745,
        .channel = 149,
        .security = RB_WIFI_SEC_WPA3,
    },
};

const rb_wifi_network_t *rb_mock_wifi_catalog(size_t *count) {
    if (count != NULL)
        *count = sizeof(g_networks) / sizeof(g_networks[0]);
    return g_networks;
}

static const rb_wifi_network_t *mock_wifi_find(const char *ssid) {
    size_t n = 0;
    const rb_wifi_network_t *nets = rb_mock_wifi_catalog(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(nets[i].ssid, ssid) == 0) return &nets[i];
    }
    return NULL;
}

rb_status_t rb_mock_wifi_get_status(rb_context_t *ctx,
                                    rb_wifi_status_t *out) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    memset(out, 0, sizeof(*out));
    out->state = st->wifi_state;
    out->diag = st->wifi_diag;
    rb_copy_str(out->ssid, sizeof(out->ssid), st->wifi_ssid);
    rb_copy_str(out->bssid, sizeof(out->bssid), st->wifi_bssid);
    rb_copy_str(out->ip_address, sizeof(out->ip_address), st->wifi_ip);
    rb_copy_str(out->gateway, sizeof(out->gateway), st->wifi_gateway);
    out->signal_dbm = st->wifi_signal_dbm;
    switch (st->wifi_state) {
        case RB_WIFI_CONNECTED:
            rb_copy_str(out->message, sizeof(out->message),
                        "connected (mock)");
            break;
        case RB_WIFI_CONNECTING:
            rb_copy_str(out->message, sizeof(out->message),
                        "connecting (mock)");
            break;
        case RB_WIFI_DISCONNECTED:
            rb_copy_str(out->message, sizeof(out->message),
                        "disconnected (mock)");
            break;
        default:
            rb_copy_str(out->message, sizeof(out->message),
                        "unavailable (mock)");
            break;
    }
    rb_mutex_unlock(&st->lock);
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    rb_context_t *ctx;
    mock_state_t *st;
} wscan_arg_t;

static void *mock_wscan_thread(void *arg) {
    wscan_arg_t *a = (wscan_arg_t *)arg;
    rb_context_t *ctx = a->ctx;
    mock_state_t *st = a->st;
    free(a);
    uint64_t start = rb_now_ms();
    size_t n = 0;
    const rb_wifi_network_t *nets = rb_mock_wifi_catalog(&n);
    /* The scan options only carry a timeout in Phase 1; the timeout is
     * read from the thread's captured copy via the shared state. */
    for (size_t i = 0; i < n; i++) {
        if (st->wscan_stop || st->stopping) break;
        if (rb_op_is_cancelled(ctx, st->wscan_op)) break;
        (void)start;
        rb_wifi_scan_callback_t cb = st->wscan_cb;
        void *ud = st->wscan_ud;
        if (cb != NULL) cb(ctx, &nets[i], ud);
        for (int s = 0; s < 6; s++) {
            if (st->wscan_stop || st->stopping) break;
            if (rb_op_is_cancelled(ctx, st->wscan_op)) break;
            rb_sleep_ms(10);
        }
    }
    rb_mutex_lock(&st->lock);
    st->wscan_running = 0;
    rb_mutex_unlock(&st->lock);
    rb_op_mark_done(ctx, st->wscan_op);
    return NULL;
}

rb_status_t rb_mock_wifi_scan_start(rb_context_t *ctx,
                                    const rb_wifi_scan_options_t *opts,
                                    rb_wifi_scan_callback_t cb, void *ud,
                                    rb_operation_id_t op) {
    (void)opts; /* Phase 1: timeout handled by caller stop/cancel. */
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    if (st->wscan_running) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_ALREADY_IN_PROGRESS;
    }
    st->wscan_cb = cb;
    st->wscan_ud = ud;
    st->wscan_op = op;
    st->wscan_stop = 0;
    st->wscan_running = 1;
    wscan_arg_t *a = (wscan_arg_t *)malloc(sizeof(*a));
    if (a == NULL) {
        st->wscan_running = 0;
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NO_MEMORY;
    }
    a->ctx = ctx;
    a->st = st;
    if (rb_thread_create(&st->wscan_thread, mock_wscan_thread, a) !=
        0) {
        free(a);
        st->wscan_running = 0;
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_mutex_unlock(&st->lock);
    return RB_OK;
}

rb_status_t rb_mock_wifi_scan_stop(rb_context_t *ctx,
                                   rb_operation_id_t op) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_thread_t th = RB_THREAD_INVALID;
    rb_mutex_lock(&st->lock);
    if (!st->wscan_running || st->wscan_op != op) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_FOUND;
    }
    st->wscan_stop = 1;
    th = st->wscan_thread;
    rb_mutex_unlock(&st->lock);
    if (!rb_thread_equal(rb_thread_self(), th)) rb_thread_join(th);
    return RB_OK;
}

void rb_mock_wifi_stop_all(mock_state_t *st) {
    rb_mutex_lock(&st->lock);
    int running = st->wscan_running;
    rb_operation_id_t op = st->wscan_op;
    rb_mutex_unlock(&st->lock);
    if (running) {
        rb_mutex_lock(&st->lock);
        st->wscan_stop = 1;
        rb_mutex_unlock(&st->lock);
        rb_thread_join(st->wscan_thread);
        rb_op_mark_done(st->ctx, op);
    }
}

/* ------------------------------------------------------------------ */
/* Connect / disconnect / quality                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    rb_context_t *ctx;
    char ssid[64];
} wconn_arg_t;

static void *mock_wconn_thread(void *arg) {
    wconn_arg_t *a = (wconn_arg_t *)arg;
    rb_context_t *ctx = a->ctx;
    mock_state_t *st = rb_mock_state(ctx);
    rb_sleep_ms(100);
    const rb_wifi_network_t *net = mock_wifi_find(a->ssid);
    rb_mutex_lock(&st->lock);
    if (st->stopping) {
        rb_mutex_unlock(&st->lock);
        free(a);
        return NULL;
    }
    if (net != NULL) {
        st->wifi_state = RB_WIFI_CONNECTED;
        st->wifi_diag = RB_WIFI_DIAG_OK;
        rb_copy_str(st->wifi_ssid, sizeof(st->wifi_ssid), net->ssid);
        rb_copy_str(st->wifi_bssid, sizeof(st->wifi_bssid), net->bssid);
        rb_copy_str(st->wifi_ip, sizeof(st->wifi_ip), "192.168.7.42");
        rb_copy_str(st->wifi_gateway, sizeof(st->wifi_gateway),
                    "192.168.7.1");
        st->wifi_signal_dbm = net->signal_dbm;
    } else {
        st->wifi_state = RB_WIFI_FAILED;
        st->wifi_diag = RB_WIFI_DIAG_FAILED;
    }
    rb_mutex_unlock(&st->lock);
    rb_emit_event(ctx, RB_EVENT_WIFI_CHANGED, RB_OK,
                  net != NULL ? "Wi-Fi connected (mock)"
                              : "Wi-Fi connect failed (mock)");
    free(a);
    return NULL;
}

rb_status_t rb_mock_wifi_connect(rb_context_t *ctx, const char *ssid,
                                 const char *password,
                                 const rb_wifi_connect_options_t *opts) {
    (void)password; /* Never logged, never retained. */
    (void)opts;
    const rb_wifi_network_t *net = mock_wifi_find(ssid);
    if (net == NULL) return RB_ERROR_NOT_FOUND;
    if (net->security != RB_WIFI_SEC_OPEN &&
        (password == NULL || password[0] == '\0'))
        return RB_ERROR_AUTH_FAILED;
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    st->wifi_state = RB_WIFI_CONNECTING;
    st->wifi_diag = RB_WIFI_DIAG_OK;
    rb_mutex_unlock(&st->lock);
    wconn_arg_t *a = (wconn_arg_t *)malloc(sizeof(*a));
    if (a == NULL) return RB_ERROR_NO_MEMORY;
    a->ctx = ctx;
    rb_copy_str(a->ssid, sizeof(a->ssid), ssid);
    rb_thread_t th;
    if (rb_thread_create(&th, mock_wconn_thread, a) != 0) {
        free(a);
        rb_mutex_lock(&st->lock);
        st->wifi_state = RB_WIFI_DISCONNECTED;
        st->wifi_diag = RB_WIFI_DIAG_DISCONNECTED;
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    return RB_OK;
}

rb_status_t rb_mock_wifi_disconnect(rb_context_t *ctx) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    int was_connected = (st->wifi_state == RB_WIFI_CONNECTED ||
                         st->wifi_state == RB_WIFI_CONNECTING);
    st->wifi_state = RB_WIFI_DISCONNECTED;
    st->wifi_diag = RB_WIFI_DIAG_DISCONNECTED;
    st->wifi_ssid[0] = '\0';
    st->wifi_bssid[0] = '\0';
    st->wifi_ip[0] = '\0';
    st->wifi_gateway[0] = '\0';
    st->wifi_signal_dbm = 0;
    rb_mutex_unlock(&st->lock);
    if (was_connected)
        rb_emit_event(ctx, RB_EVENT_WIFI_CHANGED, RB_OK,
                      "Wi-Fi disconnected (mock)");
    return RB_OK;
}

rb_status_t rb_mock_wifi_measure_quality(rb_context_t *ctx,
                                         const char *endpoint,
                                         rb_wifi_quality_t *out) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    int connected = (st->wifi_state == RB_WIFI_CONNECTED);
    int8_t signal = st->wifi_signal_dbm;
    rb_mutex_unlock(&st->lock);
    memset(out, 0, sizeof(*out));
    rb_copy_str(out->endpoint, sizeof(out->endpoint),
                endpoint != NULL && endpoint[0] != '\0' ? endpoint
                                                        : "rb.test.local");
    if (!connected) {
        out->reachable = 0;
        out->latency_ms = -1.0;
        out->jitter_ms = -1.0;
        out->packet_loss_percent = 100.0;
        return RB_ERROR_NOT_CONNECTED;
    }
    /* SIMULATED: latency grows as signal weakens. Real backends probe
     * real endpoints; the mock documents its numbers as simulated. */
    double sig = (double)(-signal); /* 45 .. 81 */
    out->reachable = 1;
    out->latency_ms = 8.0 + (sig - 40.0) * 0.8;
    if (out->latency_ms < 8.0) out->latency_ms = 8.0;
    out->jitter_ms = out->latency_ms * 0.15;
    out->packet_loss_percent = (signal < -70) ? 2.0 : 0.0;
    return RB_OK;
}

rb_status_t rb_mock_wifi_trigger_change(rb_context_t *ctx,
                                        rb_wifi_state_t state) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    st->wifi_state = state;
    switch (state) {
        case RB_WIFI_CONNECTED:
            st->wifi_diag = RB_WIFI_DIAG_OK;
            break;
        case RB_WIFI_DISCONNECTED:
            st->wifi_diag = RB_WIFI_DIAG_DISCONNECTED;
            break;
        case RB_WIFI_UNAVAILABLE:
            st->wifi_diag = RB_WIFI_DIAG_UNAVAILABLE;
            break;
        case RB_WIFI_PERMISSION_DENIED:
            st->wifi_diag = RB_WIFI_DIAG_PERMISSION_DENIED;
            break;
        default:
            st->wifi_diag = RB_WIFI_DIAG_FAILED;
            break;
    }
    rb_mutex_unlock(&st->lock);
    rb_emit_event(ctx, RB_EVENT_WIFI_CHANGED, RB_OK,
                  "Wi-Fi state changed (mock trigger)");
    return RB_OK;
}
