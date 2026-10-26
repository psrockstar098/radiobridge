/*
 * RadioBridge — rb_core.c
 *
 * Context lifecycle, logging, operation registry, capability reporting,
 * cancellation, permission reporting, and public API dispatch to the
 * active backend. The core never touches a radio: all platform work
 * lives in backends behind rb_backend_ops_t.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rb_internal.h"

/* ------------------------------------------------------------------ */
/* Status strings                                                      */
/* ------------------------------------------------------------------ */

const char *rb_status_string(rb_status_t status) {
    switch (status) {
        case RB_OK: return "OK";
        case RB_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case RB_ERROR_NOT_INITIALIZED: return "not initialized";
        case RB_ERROR_NO_MEMORY: return "out of memory";
        case RB_ERROR_NOT_SUPPORTED: return "not supported";
        case RB_ERROR_UNAVAILABLE: return "unavailable";
        case RB_ERROR_PERMISSION_DENIED: return "permission denied";
        case RB_ERROR_NOT_FOUND: return "not found";
        case RB_ERROR_TIMEOUT: return "timeout";
        case RB_ERROR_CANCELLED: return "cancelled";
        case RB_ERROR_ALREADY_IN_PROGRESS: return "already in progress";
        case RB_ERROR_NOT_CONNECTED: return "not connected";
        case RB_ERROR_BUSY: return "busy";
        case RB_ERROR_AUTH_FAILED: return "authentication failed";
        case RB_ERROR_INTERNAL: return "internal error";
        default: return "unknown status";
    }
}

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

void rb_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

uint64_t rb_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

void rb_copy_str(char *dst, size_t dst_len, const char *src) {
    if (dst_len == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* BLE address form: "AA:BB:CC:DD:EE:FF" (6 hex pairs). Apple backends
 * may instead use a platform UUID string; that form is validated by the
 * Apple backend itself, so the core accepts any non-empty string here
 * and leaves strict validation to backends. */
int rb_address_valid(const char *address) {
    return address != NULL && address[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void rb_default_logger(rb_log_level_t level, const char *subsystem,
                              const char *message, void *user_data) {
    (void)user_data;
    const char *name = "UNKNOWN";
    switch (level) {
        case RB_LOG_ERROR: name = "ERROR"; break;
        case RB_LOG_WARN: name = "WARN"; break;
        case RB_LOG_INFO: name = "INFO"; break;
        case RB_LOG_DEBUG: name = "DEBUG"; break;
        default: break;
    }
    fprintf(stderr, "radiobridge [%s] %s: %s\n", name, subsystem, message);
}

void rb_logf(rb_context_t *ctx, rb_log_level_t level,
             const char *subsystem, const char *fmt, ...) {
    if (ctx == NULL) return;
    if (level > ctx->config.log_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rb_log_callback_t cb = ctx->config.log_callback;
    if (cb != NULL) {
        cb(level, subsystem, buf, ctx->config.log_user_data);
    } else {
        rb_default_logger(level, subsystem, buf, NULL);
    }
}

void rb_emit_event(rb_context_t *ctx, rb_event_type_t type,
                   rb_status_t status, const char *message) {
    if (ctx == NULL) return;
    rb_event_callback_t cb = ctx->config.event_callback;
    if (cb == NULL) return;
    rb_event_t ev;
    ev.type = type;
    ev.status = status;
    rb_copy_str(ev.message, sizeof(ev.message), message);
    cb(ctx, &ev, ctx->config.event_user_data);
}

/* ------------------------------------------------------------------ */
/* Operation registry                                                  */
/* ------------------------------------------------------------------ */

rb_operation_id_t rb_op_register(rb_context_t *ctx) {
    pthread_mutex_lock(&ctx->lock);
    rb_operation_id_t id = ctx->next_op_id++;
    if (id == 0) id = ctx->next_op_id++; /* 0 is never a valid id */
    rb_op_entry_t *e = (rb_op_entry_t *)calloc(1, sizeof(*e));
    if (e != NULL) {
        e->id = id;
        e->next = ctx->op_list;
        ctx->op_list = e;
    }
    pthread_mutex_unlock(&ctx->lock);
    return id;
}

void rb_op_mark_done(rb_context_t *ctx, rb_operation_id_t id) {
    pthread_mutex_lock(&ctx->lock);
    rb_op_entry_t **pp = &ctx->op_list;
    while (*pp != NULL) {
        if ((*pp)->id == id) {
            rb_op_entry_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&ctx->lock);
}

int rb_op_is_cancelled(rb_context_t *ctx, rb_operation_id_t id) {
    int cancelled = 0;
    pthread_mutex_lock(&ctx->lock);
    for (rb_op_entry_t *e = ctx->op_list; e != NULL; e = e->next) {
        if (e->id == id) {
            cancelled = e->cancelled;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return cancelled;
}

/* Returns 1 when the id was found and flagged, 0 otherwise. */
int rb_op_cancel_flag(rb_context_t *ctx, rb_operation_id_t id) {
    int found = 0;
    pthread_mutex_lock(&ctx->lock);
    for (rb_op_entry_t *e = ctx->op_list; e != NULL; e = e->next) {
        if (e->id == id) {
            e->cancelled = 1;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return found;
}

/* ------------------------------------------------------------------ */
/* Context lifecycle                                                   */
/* ------------------------------------------------------------------ */

static const rb_backend_ops_t *rb_ops_for_kind(rb_backend_kind_t kind) {
    switch (kind) {
        case RB_BACKEND_MOCK: return &rb_mock_backend_ops;
        case RB_BACKEND_LINUX: return &rb_linux_backend_ops;
        case RB_BACKEND_ANDROID: return &rb_android_stub_backend_ops;
        case RB_BACKEND_APPLE: return &rb_apple_stub_backend_ops;
        case RB_BACKEND_WINDOWS: return &rb_windows_stub_backend_ops;
        case RB_BACKEND_AUTO:
        default: return NULL; /* resolved by the caller */
    }
}

static const char *rb_backend_kind_name(rb_backend_kind_t kind) {
    switch (kind) {
        case RB_BACKEND_MOCK: return "mock";
        case RB_BACKEND_LINUX: return "linux";
        case RB_BACKEND_ANDROID: return "android";
        case RB_BACKEND_APPLE: return "apple";
        case RB_BACKEND_WINDOWS: return "windows";
        case RB_BACKEND_AUTO:
        default: return "auto";
    }
}

rb_status_t rb_initialize(const rb_config_t *config,
                          rb_context_t **out_context) {
    if (out_context == NULL) return RB_ERROR_INVALID_ARGUMENT;
    *out_context = NULL;

    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_AUTO;
    cfg.log_level = RB_LOG_WARN;
    if (config != NULL) cfg = *config;

    rb_context_t *ctx = (rb_context_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) return RB_ERROR_NO_MEMORY;
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->config = cfg;
    ctx->next_op_id = 1;

    /* Backend selection. AUTO prefers the native backend for this host
     * and falls back to mock with a warning when native is unavailable.
     * An explicitly requested backend that cannot run here fails. */
    const rb_backend_ops_t *ops = NULL;
    rb_status_t st = RB_OK;
    if (cfg.backend == RB_BACKEND_AUTO) {
#if defined(__linux__)
        ops = &rb_linux_backend_ops;
        ctx->ops = ops;
        st = ops->create(ctx);
        if (st != RB_OK) {
            rb_logf(ctx, RB_LOG_WARN, "core",
                    "native linux backend unavailable (%s); falling back "
                    "to mock backend",
                    rb_status_string(st));
            ctx->ops = NULL;
            ctx->backend_state = NULL;
            ops = &rb_mock_backend_ops;
            ctx->ops = ops;
            st = ops->create(ctx);
        }
#else
        /* No native backend compiled for this host in Phase 1. */
        ops = &rb_mock_backend_ops;
        ctx->ops = ops;
        st = ops->create(ctx);
#endif
    } else {
        ops = rb_ops_for_kind(cfg.backend);
        if (ops == NULL) {
            pthread_mutex_destroy(&ctx->lock);
            free(ctx);
            return RB_ERROR_INVALID_ARGUMENT;
        }
        ctx->ops = ops;
        st = ops->create(ctx);
    }

    if (st != RB_OK) {
        rb_logf(ctx, RB_LOG_ERROR, "core",
                "backend '%s' failed to start: %s",
                ops != NULL ? ops->name : rb_backend_kind_name(cfg.backend),
                rb_status_string(st));
        pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return st;
    }

    /* Capabilities snapshot from the active backend. */
    memset(&ctx->caps, 0, sizeof(ctx->caps));
    rb_status_t pst = ctx->ops->probe(&ctx->caps);
    if (pst != RB_OK) {
        rb_logf(ctx, RB_LOG_WARN, "core",
                "backend probe failed (%s); continuing with empty caps",
                rb_status_string(pst));
    }
    rb_logf(ctx, RB_LOG_INFO, "core",
            "initialized with backend '%s' (%s)", ctx->caps.backend_name,
            ctx->caps.detail);
    *out_context = ctx;
    return RB_OK;
}

void rb_shutdown(rb_context_t *context) {
    if (context == NULL) return;
    pthread_mutex_lock(&context->lock);
    context->shutting_down = 1;
    pthread_mutex_unlock(&context->lock);
    if (context->ops != NULL && context->ops->destroy != NULL) {
        context->ops->destroy(context);
    }
    pthread_mutex_lock(&context->lock);
    rb_op_entry_t *e = context->op_list;
    while (e != NULL) {
        rb_op_entry_t *next = e->next;
        free(e);
        e = next;
    }
    context->op_list = NULL;
    pthread_mutex_unlock(&context->lock);
    pthread_mutex_destroy(&context->lock);
    free(context);
}

rb_status_t rb_get_capabilities(rb_context_t *context,
                                rb_capabilities_t *out_caps) {
    if (context == NULL || out_caps == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&context->lock);
    if (context->shutting_down) {
        pthread_mutex_unlock(&context->lock);
        return RB_ERROR_NOT_INITIALIZED;
    }
    *out_caps = context->caps;
    pthread_mutex_unlock(&context->lock);
    return RB_OK;
}

rb_status_t rb_cancel(rb_context_t *context, rb_operation_id_t operation) {
    if (context == NULL) return RB_ERROR_INVALID_ARGUMENT;
    if (operation == 0) return RB_ERROR_INVALID_ARGUMENT;
    if (!rb_op_cancel_flag(context, operation))
        return RB_ERROR_NOT_FOUND;
    /* Backends poll the flag between deliveries; scans also get an
     * explicit stop so they terminate promptly. */
    if (context->ops != NULL) {
        if (context->ops->ble_scan_stop != NULL)
            context->ops->ble_scan_stop(context, operation);
        if (context->ops->wifi_scan_stop != NULL)
            context->ops->wifi_scan_stop(context, operation);
    }
    return RB_OK;
}

const char *rb_version_string(void) {
    return "0.1.0";
}

uint32_t rb_abi_version(void) {
    return RB_API_VERSION;
}

rb_status_t rb_set_log_level(rb_context_t *context, rb_log_level_t level) {
    if (context == NULL) return RB_ERROR_INVALID_ARGUMENT;
    if (level < RB_LOG_ERROR || level > RB_LOG_DEBUG)
        return RB_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&context->lock);
    context->config.log_level = level;
    pthread_mutex_unlock(&context->lock);
    return RB_OK;
}

void rb_free(void *ptr) {
    free(ptr);
}

/* ------------------------------------------------------------------ */
/* Permission reporting                                                */
/* ------------------------------------------------------------------ */

static const char *rb_permission_name(rb_permission_mask_t bit) {
    switch (bit) {
        case RB_PERM_BLUETOOTH_SCAN: return "BLUETOOTH_SCAN";
        case RB_PERM_BLUETOOTH_CONNECT: return "BLUETOOTH_CONNECT";
        case RB_PERM_BLUETOOTH_ADVERTISE: return "BLUETOOTH_ADVERTISE";
        case RB_PERM_LOCATION: return "LOCATION";
        case RB_PERM_WIFI_SCAN: return "WIFI_SCAN";
        case RB_PERM_WIFI_CONNECT: return "WIFI_CONNECT";
        case RB_PERM_NEARBY_DEVICES: return "NEARBY_DEVICES";
        default: return "UNKNOWN";
    }
}

rb_status_t rb_get_required_permissions(rb_context_t *context,
                                        rb_operation_t operation,
                                        rb_permission_mask_t *out_mask,
                                        char (*out_names)[32],
                                        size_t *out_count) {
    if (context == NULL || out_mask == NULL || out_count == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (operation < RB_OP_BLE_SCAN || operation > RB_OP_WIFI_QUALITY)
        return RB_ERROR_INVALID_ARGUMENT;
    rb_permission_mask_t mask = RB_PERM_NONE;
    if (context->ops != NULL &&
        context->ops->required_permissions != NULL) {
        rb_status_t st =
            context->ops->required_permissions(operation, &mask);
        if (st != RB_OK) return st;
    } else {
        /* Fallback table (mock-compatible defaults). */
        switch (operation) {
            case RB_OP_BLE_SCAN:
                mask = RB_PERM_BLUETOOTH_SCAN | RB_PERM_LOCATION;
                break;
            case RB_OP_BLE_CONNECT:
            case RB_OP_BLE_READ:
            case RB_OP_BLE_WRITE:
            case RB_OP_BLE_NOTIFY:
                mask = RB_PERM_BLUETOOTH_CONNECT;
                break;
            case RB_OP_WIFI_SCAN:
                mask = RB_PERM_WIFI_SCAN | RB_PERM_LOCATION;
                break;
            case RB_OP_WIFI_CONNECT:
                mask = RB_PERM_WIFI_CONNECT;
                break;
            default:
                mask = RB_PERM_NONE;
                break;
        }
    }
    *out_mask = mask;
    size_t capacity = *out_count;
    size_t written = 0;
    static const rb_permission_mask_t bits[] = {
        RB_PERM_BLUETOOTH_SCAN, RB_PERM_BLUETOOTH_CONNECT,
        RB_PERM_BLUETOOTH_ADVERTISE, RB_PERM_LOCATION, RB_PERM_WIFI_SCAN,
        RB_PERM_WIFI_CONNECT, RB_PERM_NEARBY_DEVICES
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if ((mask & bits[i]) == 0) continue;
        if (out_names != NULL && written < capacity) {
            rb_copy_str(out_names[written], 32,
                        rb_permission_name(bits[i]));
        }
        written++;
    }
    *out_count = written;
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* UUID helpers                                                        */
/* ------------------------------------------------------------------ */

static int rb_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse 4, 8, or 32 hex digits (32-digit form may contain dashes in the
 * canonical 8-4-4-4-12 layout). Short forms expand into the Bluetooth
 * base UUID 0000xxxx-0000-1000-8000-00805F9B34FB. */
rb_status_t rb_uuid_from_string(const char *str, rb_uuid_t *out) {
    if (str == NULL || out == NULL) return RB_ERROR_INVALID_ARGUMENT;
    char hex[33];
    size_t n = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '-') continue;
        if (n >= 32) return RB_ERROR_INVALID_ARGUMENT;
        if (rb_hex_val(str[i]) < 0) return RB_ERROR_INVALID_ARGUMENT;
        hex[n++] = str[i];
    }
    hex[n] = '\0';
    uint8_t full[16];
    if (n == 4) {
        /* 16-bit short form. */
        uint16_t v = (uint16_t)((rb_hex_val(hex[0]) << 12) |
                                (rb_hex_val(hex[1]) << 8) |
                                (rb_hex_val(hex[2]) << 4) |
                                rb_hex_val(hex[3]));
        return rb_uuid_from_16(v, out);
    }
    if (n == 8) {
        /* 32-bit short form. */
        uint32_t v = 0;
        for (int i = 0; i < 8; i++)
            v = (v << 4) | (uint32_t)rb_hex_val(hex[i]);
        static const uint8_t base[16] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
            0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
        };
        memcpy(full, base, 16);
        full[0] = (uint8_t)(v >> 24);
        full[1] = (uint8_t)(v >> 16);
        full[2] = (uint8_t)(v >> 8);
        full[3] = (uint8_t)v;
        memcpy(out->bytes, full, 16);
        return RB_OK;
    }
    if (n == 32) {
        for (int i = 0; i < 16; i++) {
            int hi = rb_hex_val(hex[2 * i]);
            int lo = rb_hex_val(hex[2 * i + 1]);
            full[i] = (uint8_t)((hi << 4) | lo);
        }
        memcpy(out->bytes, full, 16);
        return RB_OK;
    }
    return RB_ERROR_INVALID_ARGUMENT;
}

rb_status_t rb_uuid_from_16(uint16_t short_uuid, rb_uuid_t *out) {
    if (out == NULL) return RB_ERROR_INVALID_ARGUMENT;
    static const uint8_t base[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb
    };
    memcpy(out->bytes, base, 16);
    out->bytes[2] = (uint8_t)(short_uuid >> 8);
    out->bytes[3] = (uint8_t)short_uuid;
    return RB_OK;
}

void rb_uuid_to_string(const rb_uuid_t *uuid, char *out, size_t out_len) {
    static const char *digits = "0123456789abcdef";
    if (uuid == NULL || out == NULL || out_len < 37) {
        if (out != NULL && out_len > 0) out[0] = '\0';
        return;
    }
    /* Canonical 8-4-4-4-12 layout. */
    size_t pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[pos++] = '-';
        out[pos++] = digits[uuid->bytes[i] >> 4];
        out[pos++] = digits[uuid->bytes[i] & 0x0f];
    }
    out[pos] = '\0';
}

int rb_uuid_equals(const rb_uuid_t *a, const rb_uuid_t *b) {
    if (a == NULL || b == NULL) return 0;
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

/* ------------------------------------------------------------------ */
/* Public dispatch: context validation + backend call                  */
/* ------------------------------------------------------------------ */

static int rb_ctx_live(rb_context_t *ctx) {
    if (ctx == NULL) return 0;
    pthread_mutex_lock(&ctx->lock);
    int live = !ctx->shutting_down && ctx->ops != NULL;
    pthread_mutex_unlock(&ctx->lock);
    return live;
}

#define RB_REQUIRE_CTX(ctx) \
    do { \
        if (!rb_ctx_live(ctx)) return RB_ERROR_NOT_INITIALIZED; \
    } while (0)

#define RB_REQUIRE_OP(ctx, opname) \
    do { \
        if ((ctx)->ops == NULL || (ctx)->ops->opname == NULL) \
            return RB_ERROR_NOT_SUPPORTED; \
    } while (0)

/* BLE dispatch. */

rb_status_t rb_ble_scan_start(rb_context_t *context,
                              const rb_ble_scan_options_t *options,
                              rb_ble_scan_callback_t callback,
                              void *user_data,
                              rb_operation_id_t *out_operation) {
    RB_REQUIRE_CTX(context);
    if (callback == NULL || out_operation == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_scan_start);
    rb_ble_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    if (options != NULL) opts = *options;
    rb_operation_id_t id = rb_op_register(context);
    rb_status_t st = context->ops->ble_scan_start(context, &opts, callback,
                                                  user_data, id);
    if (st != RB_OK) {
        rb_op_mark_done(context, id);
        return st;
    }
    *out_operation = id;
    return RB_OK;
}

rb_status_t rb_ble_scan_stop(rb_context_t *context,
                             rb_operation_id_t operation) {
    RB_REQUIRE_CTX(context);
    if (operation == 0) return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_scan_stop);
    return context->ops->ble_scan_stop(context, operation);
}

rb_status_t rb_ble_connect(rb_context_t *context,
                           const rb_device_id_t *device,
                           const rb_ble_connect_options_t *options,
                           rb_ble_connection_callback_t callback,
                           void *user_data,
                           rb_operation_id_t *out_operation) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || callback == NULL || out_operation == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (!rb_address_valid(device->address))
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_connect);
    rb_ble_connect_options_t opts;
    memset(&opts, 0, sizeof(opts));
    if (options != NULL) opts = *options;
    rb_operation_id_t id = rb_op_register(context);
    rb_status_t st = context->ops->ble_connect(context, device, &opts,
                                               callback, user_data, id);
    if (st != RB_OK) {
        rb_op_mark_done(context, id);
        return st;
    }
    *out_operation = id;
    return RB_OK;
}

rb_status_t rb_ble_disconnect(rb_context_t *context,
                              const rb_device_id_t *device) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || !rb_address_valid(device->address))
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_disconnect);
    return context->ops->ble_disconnect(context, device);
}

rb_status_t rb_ble_discover(rb_context_t *context,
                            const rb_device_id_t *device,
                            rb_ble_service_t **out_services,
                            size_t *out_count) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || out_services == NULL || out_count == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    if (!rb_address_valid(device->address))
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_discover);
    return context->ops->ble_discover(context, device, out_services,
                                      out_count);
}

rb_status_t rb_ble_read(rb_context_t *context,
                        const rb_device_id_t *device,
                        const rb_uuid_t *service_uuid,
                        const rb_uuid_t *characteristic_uuid,
                        uint8_t *out_value, size_t *out_len) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || service_uuid == NULL ||
        characteristic_uuid == NULL || out_value == NULL ||
        out_len == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_read);
    return context->ops->ble_read(context, device, service_uuid,
                                  characteristic_uuid, out_value, out_len);
}

rb_status_t rb_ble_write(rb_context_t *context,
                         const rb_device_id_t *device,
                         const rb_uuid_t *service_uuid,
                         const rb_uuid_t *characteristic_uuid,
                         const uint8_t *value, size_t value_len,
                         int with_response) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || service_uuid == NULL ||
        characteristic_uuid == NULL ||
        (value == NULL && value_len > 0))
        return RB_ERROR_INVALID_ARGUMENT;
    if (value_len > RB_BLE_MAX_VALUE_LEN)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_write);
    return context->ops->ble_write(context, device, service_uuid,
                                   characteristic_uuid, value, value_len,
                                   with_response);
}

rb_status_t rb_ble_subscribe(rb_context_t *context,
                             const rb_device_id_t *device,
                             const rb_uuid_t *service_uuid,
                             const rb_uuid_t *characteristic_uuid,
                             rb_ble_notify_callback_t callback,
                             void *user_data) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || service_uuid == NULL ||
        characteristic_uuid == NULL || callback == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_subscribe);
    return context->ops->ble_subscribe(context, device, service_uuid,
                                       characteristic_uuid, callback,
                                       user_data);
}

rb_status_t rb_ble_unsubscribe(rb_context_t *context,
                               const rb_device_id_t *device,
                               const rb_uuid_t *service_uuid,
                               const rb_uuid_t *characteristic_uuid) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || service_uuid == NULL ||
        characteristic_uuid == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_unsubscribe);
    return context->ops->ble_unsubscribe(context, device, service_uuid,
                                         characteristic_uuid);
}

rb_status_t rb_ble_get_rssi(rb_context_t *context,
                            const rb_device_id_t *device,
                            int8_t *out_rssi_dbm) {
    RB_REQUIRE_CTX(context);
    if (device == NULL || out_rssi_dbm == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_get_rssi);
    return context->ops->ble_get_rssi(context, device, out_rssi_dbm);
}

rb_status_t rb_ble_set_reconnect_policy(rb_context_t *context,
                                        const rb_reconnect_policy_t *policy) {
    RB_REQUIRE_CTX(context);
    if (policy == NULL) return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, ble_set_reconnect_policy);
    return context->ops->ble_set_reconnect_policy(context, policy);
}

/* Wi-Fi dispatch. */

rb_status_t rb_wifi_get_status(rb_context_t *context,
                               rb_wifi_status_t *out_status) {
    RB_REQUIRE_CTX(context);
    if (out_status == NULL) return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, wifi_get_status);
    return context->ops->wifi_get_status(context, out_status);
}

rb_status_t rb_wifi_scan_start(rb_context_t *context,
                               const rb_wifi_scan_options_t *options,
                               rb_wifi_scan_callback_t callback,
                               void *user_data,
                               rb_operation_id_t *out_operation) {
    RB_REQUIRE_CTX(context);
    if (callback == NULL || out_operation == NULL)
        return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, wifi_scan_start);
    rb_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    if (options != NULL) opts = *options;
    rb_operation_id_t id = rb_op_register(context);
    rb_status_t st = context->ops->wifi_scan_start(context, &opts, callback,
                                                   user_data, id);
    if (st != RB_OK) {
        rb_op_mark_done(context, id);
        return st;
    }
    *out_operation = id;
    return RB_OK;
}

rb_status_t rb_wifi_scan_stop(rb_context_t *context,
                              rb_operation_id_t operation) {
    RB_REQUIRE_CTX(context);
    if (operation == 0) return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, wifi_scan_stop);
    return context->ops->wifi_scan_stop(context, operation);
}

rb_status_t rb_wifi_connect(rb_context_t *context, const char *ssid,
                            const char *password,
                            const rb_wifi_connect_options_t *options) {
    RB_REQUIRE_CTX(context);
    if (ssid == NULL || ssid[0] == '\0')
        return RB_ERROR_INVALID_ARGUMENT;
    /* The password is deliberately never logged anywhere in this
     * library. Backends receive it only for the connection attempt. */
    RB_REQUIRE_OP(context, wifi_connect);
    (void)options;
    return context->ops->wifi_connect(context, ssid, password, options);
}

rb_status_t rb_wifi_disconnect(rb_context_t *context) {
    RB_REQUIRE_CTX(context);
    RB_REQUIRE_OP(context, wifi_disconnect);
    return context->ops->wifi_disconnect(context);
}

rb_status_t rb_wifi_measure_quality(rb_context_t *context,
                                    const char *endpoint,
                                    rb_wifi_quality_t *out_quality) {
    RB_REQUIRE_CTX(context);
    if (out_quality == NULL) return RB_ERROR_INVALID_ARGUMENT;
    RB_REQUIRE_OP(context, wifi_measure_quality);
    return context->ops->wifi_measure_quality(context, endpoint,
                                              out_quality);
}

/* Mock-only test hooks (NULL backend hook -> NOT_SUPPORTED). */

rb_status_t rb_mock_trigger_disconnect(rb_context_t *context,
                                       const rb_device_id_t *device) {
    RB_REQUIRE_CTX(context);
    if (device == NULL) return RB_ERROR_INVALID_ARGUMENT;
    if (context->ops->mock_trigger_disconnect == NULL)
        return RB_ERROR_NOT_SUPPORTED;
    return context->ops->mock_trigger_disconnect(context, device);
}

rb_status_t rb_mock_trigger_wifi_change(rb_context_t *context,
                                        rb_wifi_state_t state) {
    RB_REQUIRE_CTX(context);
    if (context->ops->mock_trigger_wifi_change == NULL)
        return RB_ERROR_NOT_SUPPORTED;
    return context->ops->mock_trigger_wifi_change(context, state);
}
