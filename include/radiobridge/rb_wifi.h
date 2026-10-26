/*
 * RadioBridge — rb_wifi.h
 *
 * Wi-Fi operations. Scanning and connection are distinct capabilities
 * everywhere: several operating systems permit connection requests while
 * restricting scans (or gating them on permissions), so the capability
 * model reports them separately and rb_wifi_scan_start() may return
 * RB_ERROR_PERMISSION_DENIED even when rb_wifi_connect() works.
 *
 * The library never logs or stores Wi-Fi passwords: the password passed
 * to rb_wifi_connect() is used only for the connection attempt and is
 * never written to any log (covered by test_logging).
 */
#ifndef RADIOBRIDGE_RB_WIFI_H
#define RADIOBRIDGE_RB_WIFI_H

#include "rb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Current Wi-Fi state and connection metadata. Never fails on a healthy
 * context; returns RB_ERROR_UNAVAILABLE when the backend has no Wi-Fi
 * service (reported, not fabricated). */
rb_status_t rb_wifi_get_status(rb_context_t *context,
                               rb_wifi_status_t *out_status);

/* Start an asynchronous Wi-Fi scan. Each found network is delivered to
 * `callback`. Runs until the timeout expires, rb_wifi_scan_stop(), or
 * rb_cancel(). A second concurrent scan returns
 * RB_ERROR_ALREADY_IN_PROGRESS. */
rb_status_t rb_wifi_scan_start(rb_context_t *context,
                               const rb_wifi_scan_options_t *options,
                               rb_wifi_scan_callback_t callback,
                               void *user_data,
                               rb_operation_id_t *out_operation);

/* Stop a running Wi-Fi scan. Unknown/finished ids return
 * RB_ERROR_NOT_FOUND. */
rb_status_t rb_wifi_scan_stop(rb_context_t *context,
                              rb_operation_id_t operation);

/* Request a connection to a network. On success the connection proceeds
 * asynchronously; the result is visible via rb_wifi_get_status() and the
 * RB_EVENT_WIFI_CHANGED event. Unknown SSIDs report
 * RB_ERROR_NOT_FOUND synchronously. `password` may be NULL for open
 * networks; it is never logged or retained after the call returns. */
rb_status_t rb_wifi_connect(rb_context_t *context, const char *ssid,
                            const char *password,
                            const rb_wifi_connect_options_t *options);

/* Disconnect the current Wi-Fi network, if any. Returns RB_OK even when
 * already disconnected. */
rb_status_t rb_wifi_disconnect(rb_context_t *context);

/* Measure connectivity quality against an endpoint. On the mock backend
 * the numbers are simulated from signal strength and documented as
 * simulated; on real backends they come from real probes. `endpoint`
 * may be NULL for the backend default. */
rb_status_t rb_wifi_measure_quality(rb_context_t *context,
                                    const char *endpoint,
                                    rb_wifi_quality_t *out_quality);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_WIFI_H */
