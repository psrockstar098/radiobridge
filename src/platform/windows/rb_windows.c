/*
 * RadioBridge — rb_windows.c (Windows backend: C vtable shim)
 *
 * COMPILE-UNVERIFIED — written from documented WinRT APIs, never
 * compiled (no Windows SDK on the authoring machine). Requires
 * Windows SDK + a Bluetooth-capable Windows device to verify.
 *
 * This file is C11 and contains no WinRT code: it adapts the C++
 * implementation (rb_windows.cpp, rb_windows_wifi.cpp) to the
 * rb_backend_ops_t vtable. It is compiled only on Windows (see the
 * CMake snippet in rb_windows_errors.md); on every other platform
 * the stub in src/platform/stubs/rb_stubs.c still stands.
 *
 * Two operations are answered here, without touching WinRT, because
 * the platform genuinely cannot do them:
 *  - ble_get_rssi(): WinRT exposes signal strength only on
 *    advertisement reports. There is no connected-device RSSI API,
 *    so this always returns RB_ERROR_NOT_SUPPORTED (and the
 *    capability reports NOT_SUPPORTED). Scan results still carry
 *    RSSI — that number is real advertisement data.
 *  - wifi_measure_quality(): WinRT has no endpoint latency /
 *    stability probe, so this always returns
 *    RB_ERROR_NOT_SUPPORTED (capability NOT_SUPPORTED).
 */
#ifndef _WIN32
#error "The Windows backend compiles on Windows only."
#endif

#include <string.h>

#include "core/rb_internal.h"
#include "rb_windows_internal.h"

static rb_status_t windows_probe(rb_capabilities_t *caps) {
    return rb_win_probe(caps);
}

static rb_status_t windows_create(rb_context_t *ctx) {
    return rb_win_create(ctx);
}

static void windows_destroy(rb_context_t *ctx) {
    rb_win_destroy(ctx);
}

static rb_status_t windows_ble_scan_start(rb_context_t *ctx,
                                          const rb_ble_scan_options_t *o,
                                          rb_ble_scan_callback_t c,
                                          void *u,
                                          rb_operation_id_t op) {
    return rb_win_ble_scan_start(ctx, o, c, u, op);
}

static rb_status_t windows_ble_scan_stop(rb_context_t *ctx,
                                         rb_operation_id_t op) {
    return rb_win_ble_scan_stop(ctx, op);
}

static rb_status_t windows_ble_connect(rb_context_t *ctx,
                                       const rb_device_id_t *d,
                                       const rb_ble_connect_options_t *o,
                                       rb_ble_connection_callback_t c,
                                       void *u, rb_operation_id_t op) {
    return rb_win_ble_connect(ctx, d, o, c, u, op);
}

static rb_status_t windows_ble_disconnect(rb_context_t *ctx,
                                          const rb_device_id_t *d) {
    return rb_win_ble_disconnect(ctx, d);
}

static rb_status_t windows_ble_discover(rb_context_t *ctx,
                                        const rb_device_id_t *d,
                                        rb_ble_service_t **o, size_t *n) {
    return rb_win_ble_discover(ctx, d, o, n);
}

static rb_status_t windows_ble_read(rb_context_t *ctx,
                                    const rb_device_id_t *d,
                                    const rb_uuid_t *s, const rb_uuid_t *c,
                                    uint8_t *v, size_t *n) {
    return rb_win_ble_read(ctx, d, s, c, v, n);
}

static rb_status_t windows_ble_write(rb_context_t *ctx,
                                     const rb_device_id_t *d,
                                     const rb_uuid_t *s, const rb_uuid_t *c,
                                     const uint8_t *v, size_t n, int w) {
    return rb_win_ble_write(ctx, d, s, c, v, n, w);
}

static rb_status_t windows_ble_subscribe(rb_context_t *ctx,
                                         const rb_device_id_t *d,
                                         const rb_uuid_t *s,
                                         const rb_uuid_t *c,
                                         rb_ble_notify_callback_t cb,
                                         void *u) {
    return rb_win_ble_subscribe(ctx, d, s, c, cb, u);
}

static rb_status_t windows_ble_unsubscribe(rb_context_t *ctx,
                                           const rb_device_id_t *d,
                                           const rb_uuid_t *s,
                                           const rb_uuid_t *c) {
    return rb_win_ble_unsubscribe(ctx, d, s, c);
}

/* No connected-device RSSI API exists in WinRT. Never fabricate one. */
static rb_status_t windows_ble_get_rssi(rb_context_t *ctx,
                                        const rb_device_id_t *d,
                                        int8_t *r) {
    (void)ctx;
    (void)d;
    (void)r;
    return RB_ERROR_NOT_SUPPORTED;
}

static rb_status_t windows_ble_set_reconnect_policy(
    rb_context_t *ctx, const rb_reconnect_policy_t *p) {
    return rb_win_ble_set_reconnect_policy(ctx, p);
}

static rb_status_t windows_wifi_get_status(rb_context_t *ctx,
                                           rb_wifi_status_t *o) {
    return rb_win_wifi_get_status(ctx, o);
}

static rb_status_t windows_wifi_scan_start(rb_context_t *ctx,
                                           const rb_wifi_scan_options_t *o,
                                           rb_wifi_scan_callback_t c,
                                           void *u, rb_operation_id_t op) {
    return rb_win_wifi_scan_start(ctx, o, c, u, op);
}

static rb_status_t windows_wifi_scan_stop(rb_context_t *ctx,
                                          rb_operation_id_t op) {
    return rb_win_wifi_scan_stop(ctx, op);
}

static rb_status_t windows_wifi_connect(rb_context_t *ctx, const char *s,
                                        const char *p,
                                        const rb_wifi_connect_options_t *o) {
    return rb_win_wifi_connect(ctx, s, p, o);
}

static rb_status_t windows_wifi_disconnect(rb_context_t *ctx) {
    return rb_win_wifi_disconnect(ctx);
}

/* No endpoint quality probe exists in the WinRT Wi-Fi surface. */
static rb_status_t windows_wifi_measure_quality(rb_context_t *ctx,
                                                const char *e,
                                                rb_wifi_quality_t *o) {
    (void)ctx;
    (void)e;
    (void)o;
    return RB_ERROR_NOT_SUPPORTED;
}

/* Permission reporting. The library never requests permissions; it
 * reports what the OS flow needs (see rb_types.h). Windows has no
 * Android-style runtime permission prompt for Bluetooth, but the
 * portable bits still describe the gates that exist: a packaged app
 * must declare the bluetooth device capability, and Wi-Fi BSSID
 * access is gated on location consent (hence LOCATION on scan).
 * Wi-Fi scan/connect additionally require the wifiControl capability
 * in a packaged manifest (see rb_windows_errors.md). */
static rb_status_t windows_required_permissions(rb_operation_t op,
                                                rb_permission_mask_t *mask) {
    if (mask == NULL) return RB_ERROR_INVALID_ARGUMENT;
    switch (op) {
        case RB_OP_BLE_SCAN:
            *mask = RB_PERM_BLUETOOTH_SCAN;
            break;
        case RB_OP_BLE_CONNECT:
        case RB_OP_BLE_READ:
        case RB_OP_BLE_WRITE:
        case RB_OP_BLE_NOTIFY:
            *mask = RB_PERM_BLUETOOTH_CONNECT;
            break;
        case RB_OP_WIFI_SCAN:
            *mask = RB_PERM_WIFI_SCAN | RB_PERM_LOCATION;
            break;
        case RB_OP_WIFI_CONNECT:
            *mask = RB_PERM_WIFI_CONNECT;
            break;
        case RB_OP_WIFI_STATUS:
        case RB_OP_WIFI_QUALITY:
        default:
            *mask = RB_PERM_NONE;
            break;
    }
    return RB_OK;
}

const rb_backend_ops_t rb_windows_backend_ops = {
    .name = "windows",
    .version = "0.2.0",
    .probe = windows_probe,
    .create = windows_create,
    .destroy = windows_destroy,
    .ble_scan_start = windows_ble_scan_start,
    .ble_scan_stop = windows_ble_scan_stop,
    .ble_connect = windows_ble_connect,
    .ble_disconnect = windows_ble_disconnect,
    .ble_discover = windows_ble_discover,
    .ble_read = windows_ble_read,
    .ble_write = windows_ble_write,
    .ble_subscribe = windows_ble_subscribe,
    .ble_unsubscribe = windows_ble_unsubscribe,
    .ble_get_rssi = windows_ble_get_rssi,
    .ble_set_reconnect_policy = windows_ble_set_reconnect_policy,
    .wifi_get_status = windows_wifi_get_status,
    .wifi_scan_start = windows_wifi_scan_start,
    .wifi_scan_stop = windows_wifi_scan_stop,
    .wifi_connect = windows_wifi_connect,
    .wifi_disconnect = windows_wifi_disconnect,
    .wifi_measure_quality = windows_wifi_measure_quality,
    .required_permissions = windows_required_permissions,
    .mock_trigger_disconnect = NULL,
    .mock_trigger_wifi_change = NULL,
};
