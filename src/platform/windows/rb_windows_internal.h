/*
 * RadioBridge — rb_windows_internal.h
 *
 * COMPILE-UNVERIFIED — written from documented WinRT APIs, never
 * compiled (no Windows SDK on the authoring machine). Requires
 * Windows SDK + a Bluetooth-capable Windows device to verify.
 *
 * C-linkage bridge between the C vtable shim (rb_windows.c) and the
 * C++/WinRT implementation (rb_windows.cpp, rb_windows_wifi.cpp).
 * This header contains no WinRT types: the C file must stay compilable
 * as C11 under MSVC, and only the .cpp files include the C++/WinRT
 * projection headers. The C++ side keeps all WinRT objects behind
 * these functions; no WinRT handle ever crosses into the C shim or
 * the public API.
 *
 * Threading contract (see rb_windows.cpp): every function below may
 * be called from any library thread. Functions that must resolve a
 * WinRT async operation run that work on a backend worker thread
 * (rb_thread_t) and never block the caller on a WinRT async object
 * directly — the caller waits, at most, on a Win32 completion event
 * with a documented timeout. Asynchronous operations (scan, connect)
 * deliver their results through the user callbacks from worker or
 * WinRT thread-pool threads, never from the caller's thread.
 */
#ifndef RADIOBRIDGE_RB_WINDOWS_INTERNAL_H
#define RADIOBRIDGE_RB_WINDOWS_INTERNAL_H

#include "core/rb_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the radios and fill caps. Implemented in rb_windows.cpp; it
 * runs the WinRT probe on a worker thread so the caller never blocks
 * on a WinRT async operation. Returns RB_OK when the probe itself
 * ran; availability lives in caps, as with every other backend. */
rb_status_t rb_win_probe(rb_capabilities_t *caps);

/* Allocate backend state into ctx->backend_state. Returns
 * RB_ERROR_UNAVAILABLE when neither a Bluetooth adapter nor a Wi-Fi
 * adapter is usable on this machine. */
rb_status_t rb_win_create(rb_context_t *ctx);

/* Signal every worker to stop, release the state anchor, and clear
 * ctx->backend_state. Detached workers that are still inside an
 * uncancellable WinRT operation keep the state alive through their
 * own reference and exit quietly without touching the freed context
 * (see the ctx_live gate in rb_windows.cpp). */
void rb_win_destroy(rb_context_t *ctx);

/* BLE operations (signatures mirror rb_backend_ops_t). */
rb_status_t rb_win_ble_scan_start(rb_context_t *ctx,
                                  const rb_ble_scan_options_t *opts,
                                  rb_ble_scan_callback_t cb, void *ud,
                                  rb_operation_id_t op);
rb_status_t rb_win_ble_scan_stop(rb_context_t *ctx, rb_operation_id_t op);
rb_status_t rb_win_ble_connect(rb_context_t *ctx,
                               const rb_device_id_t *dev,
                               const rb_ble_connect_options_t *opts,
                               rb_ble_connection_callback_t cb, void *ud,
                               rb_operation_id_t op);
rb_status_t rb_win_ble_disconnect(rb_context_t *ctx,
                                  const rb_device_id_t *dev);
rb_status_t rb_win_ble_discover(rb_context_t *ctx,
                                const rb_device_id_t *dev,
                                rb_ble_service_t **out, size_t *count);
rb_status_t rb_win_ble_read(rb_context_t *ctx, const rb_device_id_t *dev,
                            const rb_uuid_t *svc, const rb_uuid_t *chr,
                            uint8_t *out_value, size_t *out_len);
rb_status_t rb_win_ble_write(rb_context_t *ctx, const rb_device_id_t *dev,
                             const rb_uuid_t *svc, const rb_uuid_t *chr,
                             const uint8_t *value, size_t len,
                             int with_response);
rb_status_t rb_win_ble_subscribe(rb_context_t *ctx,
                                 const rb_device_id_t *dev,
                                 const rb_uuid_t *svc, const rb_uuid_t *chr,
                                 rb_ble_notify_callback_t cb, void *ud);
rb_status_t rb_win_ble_unsubscribe(rb_context_t *ctx,
                                   const rb_device_id_t *dev,
                                   const rb_uuid_t *svc, const rb_uuid_t *chr);
rb_status_t rb_win_ble_set_reconnect_policy(
    rb_context_t *ctx, const rb_reconnect_policy_t *policy);

/* Wi-Fi operations (signatures mirror rb_backend_ops_t). */
rb_status_t rb_win_wifi_get_status(rb_context_t *ctx,
                                   rb_wifi_status_t *out);
rb_status_t rb_win_wifi_scan_start(rb_context_t *ctx,
                                   const rb_wifi_scan_options_t *opts,
                                   rb_wifi_scan_callback_t cb, void *ud,
                                   rb_operation_id_t op);
rb_status_t rb_win_wifi_scan_stop(rb_context_t *ctx, rb_operation_id_t op);
rb_status_t rb_win_wifi_connect(rb_context_t *ctx, const char *ssid,
                                const char *password,
                                const rb_wifi_connect_options_t *opts);
rb_status_t rb_win_wifi_disconnect(rb_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_WINDOWS_INTERNAL_H */
