/*
 * RadioBridge — rb_internal.h
 *
 * Internal backend interface and core helpers. NOT part of the public
 * ABI: tests and backends include this, applications must not.
 *
 * Every backend implements rb_backend_ops_t. The core owns the context,
 * the operation registry, logging, and event dispatch; backends own
 * their native handles and worker threads, and must never expose a
 * platform object to the public API.
 */
#ifndef RADIOBRIDGE_RB_INTERNAL_H
#define RADIOBRIDGE_RB_INTERNAL_H

#include "core/rb_thread.h"

#include "radiobridge/rb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Backend interface                                                   */
/* ------------------------------------------------------------------ */

typedef struct rb_backend_ops rb_backend_ops_t;

struct rb_backend_ops {
    const char *name;       /* "mock", "linux", "android", ... */
    const char *version;    /* backend implementation version */
    /* Probe the platform and fill caps + detail. Returns RB_OK when the
     * probe itself ran; availability lives in caps/detail, not in the
     * return code. Backends that cannot run on this host return
     * RB_ERROR_NOT_SUPPORTED from create() instead (see below). */
    rb_status_t (*probe)(rb_capabilities_t *caps);
    /* Allocate per-context backend state and store it in
     * ctx->backend_state. Returns RB_ERROR_UNAVAILABLE when the platform
     * service is absent, RB_ERROR_NOT_SUPPORTED when this backend can
     * never run here (missing SDK/hardware). On failure ctx is left
     * without a backend and rb_initialize() fails cleanly. */
    rb_status_t (*create)(rb_context_t *ctx);
    /* Stop all backend threads and free backend state. Called once from
     * rb_shutdown(). */
    void (*destroy)(rb_context_t *ctx);

    /* BLE operations (NULL = backend does not implement; core maps NULL
     * to RB_ERROR_NOT_SUPPORTED). */
    rb_status_t (*ble_scan_start)(rb_context_t *ctx,
                                  const rb_ble_scan_options_t *opts,
                                  rb_ble_scan_callback_t cb, void *ud,
                                  rb_operation_id_t op);
    rb_status_t (*ble_scan_stop)(rb_context_t *ctx, rb_operation_id_t op);
    rb_status_t (*ble_connect)(rb_context_t *ctx,
                               const rb_device_id_t *dev,
                               const rb_ble_connect_options_t *opts,
                               rb_ble_connection_callback_t cb, void *ud,
                               rb_operation_id_t op);
    rb_status_t (*ble_disconnect)(rb_context_t *ctx,
                                  const rb_device_id_t *dev);
    rb_status_t (*ble_discover)(rb_context_t *ctx,
                                const rb_device_id_t *dev,
                                rb_ble_service_t **out, size_t *count);
    rb_status_t (*ble_read)(rb_context_t *ctx, const rb_device_id_t *dev,
                            const rb_uuid_t *svc, const rb_uuid_t *chr,
                            uint8_t *out_value, size_t *out_len);
    rb_status_t (*ble_write)(rb_context_t *ctx, const rb_device_id_t *dev,
                             const rb_uuid_t *svc, const rb_uuid_t *chr,
                             const uint8_t *value, size_t len,
                             int with_response);
    rb_status_t (*ble_subscribe)(rb_context_t *ctx,
                                 const rb_device_id_t *dev,
                                 const rb_uuid_t *svc, const rb_uuid_t *chr,
                                 rb_ble_notify_callback_t cb, void *ud);
    rb_status_t (*ble_unsubscribe)(rb_context_t *ctx,
                                   const rb_device_id_t *dev,
                                   const rb_uuid_t *svc, const rb_uuid_t *chr);
    rb_status_t (*ble_get_rssi)(rb_context_t *ctx,
                                const rb_device_id_t *dev,
                                int8_t *out_rssi_dbm);
    rb_status_t (*ble_set_reconnect_policy)(rb_context_t *ctx,
                                            const rb_reconnect_policy_t *p);

    /* Wi-Fi operations. */
    rb_status_t (*wifi_get_status)(rb_context_t *ctx,
                                   rb_wifi_status_t *out);
    rb_status_t (*wifi_scan_start)(rb_context_t *ctx,
                                   const rb_wifi_scan_options_t *opts,
                                   rb_wifi_scan_callback_t cb, void *ud,
                                   rb_operation_id_t op);
    rb_status_t (*wifi_scan_stop)(rb_context_t *ctx, rb_operation_id_t op);
    rb_status_t (*wifi_connect)(rb_context_t *ctx, const char *ssid,
                                const char *password,
                                const rb_wifi_connect_options_t *opts);
    rb_status_t (*wifi_disconnect)(rb_context_t *ctx);
    rb_status_t (*wifi_measure_quality)(rb_context_t *ctx,
                                        const char *endpoint,
                                        rb_wifi_quality_t *out);

    /* Permission reporting for one operation. */
    rb_status_t (*required_permissions)(rb_operation_t op,
                                        rb_permission_mask_t *mask);

    /* Mock-only test hooks (NULL on every other backend). */
    rb_status_t (*mock_trigger_disconnect)(rb_context_t *ctx,
                                           const rb_device_id_t *dev);
    rb_status_t (*mock_trigger_wifi_change)(rb_context_t *ctx,
                                            rb_wifi_state_t state);
};

/* Backend registry entries (defined by each backend's .c file). */
extern const rb_backend_ops_t rb_mock_backend_ops;
extern const rb_backend_ops_t rb_linux_backend_ops;
extern const rb_backend_ops_t rb_android_stub_backend_ops;
extern const rb_backend_ops_t rb_apple_stub_backend_ops;
extern const rb_backend_ops_t rb_windows_stub_backend_ops;

/* Direct probe entry points (used by tests and by rb_initialize). */
rb_status_t rb_linux_probe(rb_capabilities_t *caps);
rb_status_t rb_mock_probe(rb_capabilities_t *caps);

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

typedef struct rb_op_entry {
    rb_operation_id_t id;
    int cancelled;
    int done;
    struct rb_op_entry *next;
} rb_op_entry_t;

struct rb_context {
    rb_mutex_t lock;
    rb_config_t config;
    const rb_backend_ops_t *ops;
    void *backend_state;
    rb_capabilities_t caps;
    rb_op_entry_t *op_list;
    rb_operation_id_t next_op_id;
    int shutting_down;
};

/* ------------------------------------------------------------------ */
/* Core helpers (for backends)                                         */
/* ------------------------------------------------------------------ */

/* Logging. Never pass passwords, pairing secrets, or private device
 * data as log arguments — the core does not redact. */
void rb_logf(rb_context_t *ctx, rb_log_level_t level,
             const char *subsystem, const char *fmt, ...);

/* Emit a general event through the configured event callback. */
void rb_emit_event(rb_context_t *ctx, rb_event_type_t type,
                   rb_status_t status, const char *message);

/* Operation registry. */
rb_operation_id_t rb_op_register(rb_context_t *ctx);
void rb_op_mark_done(rb_context_t *ctx, rb_operation_id_t id);
int rb_op_is_cancelled(rb_context_t *ctx, rb_operation_id_t id);
int rb_op_cancel_flag(rb_context_t *ctx, rb_operation_id_t id);

/* Small utilities. */
void rb_sleep_ms(unsigned int ms);
uint64_t rb_now_ms(void);
void rb_copy_str(char *dst, size_t dst_len, const char *src);
int rb_address_valid(const char *address);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_INTERNAL_H */
