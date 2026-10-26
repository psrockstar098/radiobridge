/*
 * RadioBridge — rb_bluetooth.h
 *
 * Bluetooth Low Energy operations. All operations are backend-dispatched:
 * the mock backend implements them fully; native backends implement them
 * against their platform stack (BlueZ, Android, Core Bluetooth, WinRT)
 * and report RB_CAP_NOT_SUPPORTED / RB_ERROR_UNAVAILABLE for the pieces
 * their platform cannot do (see docs/capabilities.md).
 *
 * Threading: scan/connect callbacks fire from internal threads. Synchronous
 * GATT calls (discover/read/write) block briefly on the calling thread
 * while the backend resolves them. Callbacks must be thread-safe and must
 * not call rb_shutdown() or rb_ble_scan_stop() on their own operation
 * from inside the callback — stop from another thread or use rb_cancel()
 * from the owning thread.
 *
 * Ownership: structs passed to callbacks are valid only during the
 * callback. Arrays returned by rb_ble_discover() are heap-allocated and
 * freed with rb_free().
 */
#ifndef RADIOBRIDGE_RB_BLUETOOTH_H
#define RADIOBRIDGE_RB_BLUETOOTH_H

#include "rb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UUID helpers. rb_uuid_from_string accepts 16-bit ("180D"), 32-bit, and
 * 128-bit ("0000180D-0000-1000-8000-00805F9B34FB") forms. Returns
 * RB_ERROR_INVALID_ARGUMENT on malformed input. */
rb_status_t rb_uuid_from_string(const char *str, rb_uuid_t *out);
rb_status_t rb_uuid_from_16(uint16_t short_uuid, rb_uuid_t *out);
void rb_uuid_to_string(const rb_uuid_t *uuid, char *out, size_t out_len);
int rb_uuid_equals(const rb_uuid_t *a, const rb_uuid_t *b);

/* Start an asynchronous BLE scan. On success, *out_operation receives an
 * operation id usable with rb_cancel() / rb_ble_scan_stop(). Each matching
 * device is delivered to `callback` as it is found. The scan runs until
 * the timeout expires, rb_ble_scan_stop() is called, or rb_cancel() is
 * called with the operation id. Starting a second scan while one runs
 * returns RB_ERROR_ALREADY_IN_PROGRESS. */
rb_status_t rb_ble_scan_start(rb_context_t *context,
                              const rb_ble_scan_options_t *options,
                              rb_ble_scan_callback_t callback,
                              void *user_data,
                              rb_operation_id_t *out_operation);

/* Stop a running scan. Safe to call with an unknown or finished id:
 * returns RB_ERROR_NOT_FOUND in that case. */
rb_status_t rb_ble_scan_stop(rb_context_t *context,
                             rb_operation_id_t operation);

/* Connect to a device. Returns immediately with RB_OK once the connect
 * attempt has been accepted; the outcome arrives via `callback`
 * (CONNECTING -> CONNECTED, or FAILED with a status). A well-formed but
 * unknown address therefore reports RB_ERROR_NOT_FOUND asynchronously,
 * mirroring real stacks. Set options to NULL for defaults. */
rb_status_t rb_ble_connect(rb_context_t *context,
                           const rb_device_id_t *device,
                           const rb_ble_connect_options_t *options,
                           rb_ble_connection_callback_t callback,
                           void *user_data,
                           rb_operation_id_t *out_operation);

/* Disconnect a connected device. Returns RB_ERROR_NOT_CONNECTED when
 * the device is not connected. Connection callbacks are NOT invoked for
 * an explicit disconnect (the caller already knows); the state change
 * is still reported through the general event callback. */
rb_status_t rb_ble_disconnect(rb_context_t *context,
                              const rb_device_id_t *device);

/* Discover GATT services/characteristics/descriptors for a connected
 * device. Synchronous: on success *out_services points to a heap array
 * of *out_count services; free it with rb_free(). Returns
 * RB_ERROR_NOT_CONNECTED when the device is not connected. */
rb_status_t rb_ble_discover(rb_context_t *context,
                            const rb_device_id_t *device,
                            rb_ble_service_t **out_services,
                            size_t *out_count);

/* Read a characteristic value. On success *out_len bytes are written to
 * out_value (capacity must be >= RB_BLE_MAX_VALUE_LEN). Characteristics
 * without the READ property return RB_ERROR_NOT_SUPPORTED. */
rb_status_t rb_ble_read(rb_context_t *context,
                        const rb_device_id_t *device,
                        const rb_uuid_t *service_uuid,
                        const rb_uuid_t *characteristic_uuid,
                        uint8_t *out_value, size_t *out_len);

/* Write a characteristic value. `with_response` selects write-with-
 * response (reliable) vs write-without-response (command). A
 * characteristic lacking the matching property returns
 * RB_ERROR_NOT_SUPPORTED. */
rb_status_t rb_ble_write(rb_context_t *context,
                         const rb_device_id_t *device,
                         const rb_uuid_t *service_uuid,
                         const rb_uuid_t *characteristic_uuid,
                         const uint8_t *value, size_t value_len,
                         int with_response);

/* Subscribe to notifications (or indications, when the characteristic
 * offers indicate instead of notify). Data arrives via `callback` until
 * rb_ble_unsubscribe() or disconnect. Returns RB_ERROR_NOT_SUPPORTED
 * when the characteristic offers neither. */
rb_status_t rb_ble_subscribe(rb_context_t *context,
                             const rb_device_id_t *device,
                             const rb_uuid_t *service_uuid,
                             const rb_uuid_t *characteristic_uuid,
                             rb_ble_notify_callback_t callback,
                             void *user_data);

/* Cancel a subscription. Returns RB_ERROR_NOT_FOUND when no matching
 * subscription exists. */
rb_status_t rb_ble_unsubscribe(rb_context_t *context,
                               const rb_device_id_t *device,
                               const rb_uuid_t *service_uuid,
                               const rb_uuid_t *characteristic_uuid);

/* Read the current RSSI for a connected device. Backends without an RSSI
 * read (notably WinRT, which exposes no RSSI API) return
 * RB_ERROR_NOT_SUPPORTED and report RB_CAP_NOT_SUPPORTED. */
rb_status_t rb_ble_get_rssi(rb_context_t *context,
                            const rb_device_id_t *device,
                            int8_t *out_rssi_dbm);

/* Replace the managed reconnection policy (default: enabled, 3 attempts,
 * 500 ms initial delay, 2x backoff). Applies to future connections. */
rb_status_t rb_ble_set_reconnect_policy(rb_context_t *context,
                                        const rb_reconnect_policy_t *policy);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_BLUETOOTH_H */
