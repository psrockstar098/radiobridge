/*
 * RadioBridge — rb_mock_state.h
 *
 * Shared mock-backend state (BLE + Wi-Fi halves live in
 * src/bluetooth/rb_mock_ble.c and src/wifi/rb_mock_wifi.c, but the
 * context holds a single backend_state pointer, so both halves share
 * this struct). Internal only.
 */
#ifndef RADIOBRIDGE_RB_MOCK_STATE_H
#define RADIOBRIDGE_RB_MOCK_STATE_H

#include "core/rb_thread.h"

#include "rb_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mock_conn {
    char address[32];
    rb_ble_connection_state_t state;
    rb_ble_connection_callback_t cb;
    void *ud;
    rb_operation_id_t op;
    int auto_reconnect;
    uint32_t max_reconnect_attempts;
    uint32_t reconnect_delay_ms;
    rb_thread_t reconnect_thread;
    int reconnect_running;
    volatile int reconnect_stop;
    struct mock_conn *next;
} mock_conn_t;

typedef struct mock_sub {
    char address[32];
    rb_uuid_t service_uuid;
    rb_uuid_t characteristic_uuid;
    rb_ble_notify_callback_t cb;
    void *ud;
    rb_thread_t thread;
    int running;
    volatile int stop;
    struct mock_sub *next;
} mock_sub_t;

typedef struct mock_state {
    rb_mutex_t lock;
    volatile int stopping;
    rb_context_t *ctx;

    /* BLE scan job. */
    rb_thread_t scan_thread;
    int scan_running;
    volatile int scan_stop;
    rb_operation_id_t scan_op;
    rb_ble_scan_callback_t scan_cb;
    void *scan_ud;
    rb_ble_scan_options_t scan_opts;

    /* BLE connections + subscriptions. */
    mock_conn_t *conns;
    mock_sub_t *subs;
    rb_reconnect_policy_t policy;

    /* Writable echo characteristic storage (demo tag). */
    uint8_t echo_value[RB_BLE_MAX_VALUE_LEN];
    size_t echo_len;

    /* Wi-Fi state. */
    rb_wifi_state_t wifi_state;
    rb_wifi_diag_t wifi_diag;
    char wifi_ssid[64];
    char wifi_bssid[32];
    char wifi_ip[64];
    char wifi_gateway[64];
    int8_t wifi_signal_dbm;

    /* Wi-Fi scan job. */
    rb_thread_t wscan_thread;
    int wscan_running;
    volatile int wscan_stop;
    rb_operation_id_t wscan_op;
    rb_wifi_scan_callback_t wscan_cb;
    void *wscan_ud;
} mock_state_t;

static inline mock_state_t *rb_mock_state(rb_context_t *ctx) {
    return (mock_state_t *)ctx->backend_state;
}

/* Catalog accessors (implemented in rb_mock_ble.c / rb_mock_wifi.c). */
const rb_ble_device_t *rb_mock_ble_catalog(size_t *count);
const rb_wifi_network_t *rb_mock_wifi_catalog(size_t *count);

/* Wi-Fi scan teardown helper used by mock destroy (rb_mock_ble.c). */
void rb_mock_wifi_stop_all(mock_state_t *st);

/* Mock Wi-Fi change hook (wired into the ops table in rb_mock_ble.c). */
rb_status_t rb_mock_wifi_trigger_change(rb_context_t *ctx,
                                        rb_wifi_state_t state);

/* Wi-Fi ops implemented in rb_mock_wifi.c (referenced by the ops table). */
rb_status_t rb_mock_wifi_get_status(rb_context_t *ctx,
                                    rb_wifi_status_t *out);
rb_status_t rb_mock_wifi_scan_start(rb_context_t *ctx,
                                    const rb_wifi_scan_options_t *opts,
                                    rb_wifi_scan_callback_t cb, void *ud,
                                    rb_operation_id_t op);
rb_status_t rb_mock_wifi_scan_stop(rb_context_t *ctx, rb_operation_id_t op);
rb_status_t rb_mock_wifi_connect(rb_context_t *ctx, const char *ssid,
                                 const char *password,
                                 const rb_wifi_connect_options_t *opts);
rb_status_t rb_mock_wifi_disconnect(rb_context_t *ctx);
rb_status_t rb_mock_wifi_measure_quality(rb_context_t *ctx,
                                         const char *endpoint,
                                         rb_wifi_quality_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_MOCK_STATE_H */
