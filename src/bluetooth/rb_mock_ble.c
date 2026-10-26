/*
 * RadioBridge — rb_mock_ble.c (BLE half of the mock backend)
 *
 * Deterministic fake BLE radios: no hardware, no BlueZ, no daemon.
 * Every result is synthetic but structurally real: the same structs,
 * filters, state machine, GATT tables, and reconnect policy the native
 * backends must honor. Nothing here pretends to be a physical radio —
 * the backend name is "mock" and capabilities say so.
 *
 * Fake addresses use the locally-administered pattern AA:BB:CC:00:00:0N
 * so they can never be mistaken for real hardware.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rb_mock_state.h"

/* ------------------------------------------------------------------ */
/* Catalog                                                             */
/* ------------------------------------------------------------------ */

#define MOCK_NDEV 3

typedef struct {
    rb_uuid_t uuid;
    uint16_t handle;
    uint32_t props;
    rb_uuid_t descs[2];
    size_t desc_count;
} mock_char_def_t;

typedef struct {
    rb_uuid_t uuid;
    uint16_t handle;
    mock_char_def_t chars[4];
    size_t char_count;
} mock_svc_def_t;

typedef struct {
    rb_ble_device_t dev;
    mock_svc_def_t svcs[3];
    size_t svc_count;
} mock_device_def_t;

static mock_device_def_t g_catalog[MOCK_NDEV];
static rb_once_t g_catalog_once = RB_ONCE_INIT;

static rb_uuid_t u16(uint16_t v) {
    rb_uuid_t u;
    rb_uuid_from_16(v, &u);
    return u;
}

static void build_catalog(void) {
    memset(g_catalog, 0, sizeof(g_catalog));

    /* Device 1: heart-rate monitor. */
    mock_device_def_t *d = &g_catalog[0];
    rb_copy_str(d->dev.address, sizeof(d->dev.address),
                "AA:BB:CC:00:00:01");
    rb_copy_str(d->dev.name, sizeof(d->dev.name), "RB Heart Rate");
    d->dev.rssi_dbm = -52;
    d->dev.has_tx_power = 1;
    d->dev.tx_power_dbm = -20;
    d->dev.connectable = 1;
    d->dev.service_uuids[0] = u16(0x180D);
    d->dev.service_uuids[1] = u16(0x180F);
    d->dev.service_uuid_count = 2;
    d->dev.has_manufacturer_data = 1;
    d->dev.manufacturer_id = 0x1234;
    d->dev.manufacturer_data[0] = 0x01;
    d->dev.manufacturer_data[1] = 0x02;
    d->dev.manufacturer_data[2] = 0x03;
    d->dev.manufacturer_data_len = 3;
    d->svc_count = 2;
    mock_svc_def_t *s = &d->svcs[0];
    s->uuid = u16(0x180D);
    s->handle = 1;
    s->char_count = 2;
    s->chars[0].uuid = u16(0x2A37);
    s->chars[0].handle = 2;
    s->chars[0].props = RB_GATT_PROP_NOTIFY;
    s->chars[0].descs[0] = u16(0x2902);
    s->chars[0].desc_count = 1;
    s->chars[1].uuid = u16(0x2A38);
    s->chars[1].handle = 3;
    s->chars[1].props = RB_GATT_PROP_READ;
    s = &d->svcs[1];
    s->uuid = u16(0x180F);
    s->handle = 4;
    s->char_count = 1;
    s->chars[0].uuid = u16(0x2A19);
    s->chars[0].handle = 5;
    s->chars[0].props = RB_GATT_PROP_READ | RB_GATT_PROP_NOTIFY;
    s->chars[0].descs[0] = u16(0x2902);
    s->chars[0].desc_count = 1;

    /* Device 2: thermometer beacon. */
    d = &g_catalog[1];
    rb_copy_str(d->dev.address, sizeof(d->dev.address),
                "AA:BB:CC:00:00:02");
    rb_copy_str(d->dev.name, sizeof(d->dev.name), "RB Thermo Beacon");
    d->dev.rssi_dbm = -68;
    d->dev.connectable = 1;
    d->dev.service_uuids[0] = u16(0x1809);
    d->dev.service_uuid_count = 1;
    d->dev.has_manufacturer_data = 1;
    d->dev.manufacturer_id = 0x004C;
    d->dev.manufacturer_data[0] = 0x02;
    d->dev.manufacturer_data[1] = 0x15;
    d->dev.manufacturer_data_len = 2;
    d->svc_count = 1;
    s = &d->svcs[0];
    s->uuid = u16(0x1809);
    s->handle = 1;
    s->char_count = 2;
    s->chars[0].uuid = u16(0x2A1C);
    s->chars[0].handle = 2;
    s->chars[0].props = RB_GATT_PROP_READ | RB_GATT_PROP_NOTIFY;
    s->chars[0].descs[0] = u16(0x2902);
    s->chars[0].desc_count = 1;
    s->chars[1].uuid = u16(0x2A1D);
    s->chars[1].handle = 3;
    s->chars[1].props = RB_GATT_PROP_READ;

    /* Device 3: demo tag with a custom 128-bit service + echo char. */
    d = &g_catalog[2];
    rb_copy_str(d->dev.address, sizeof(d->dev.address),
                "AA:BB:CC:00:00:03");
    rb_copy_str(d->dev.name, sizeof(d->dev.name), "RB Demo Tag");
    d->dev.rssi_dbm = -81;
    d->dev.connectable = 1;
    rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345678",
                        &d->dev.service_uuids[0]);
    d->dev.service_uuid_count = 1;
    d->svc_count = 1;
    s = &d->svcs[0];
    rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345678", &s->uuid);
    s->handle = 1;
    s->char_count = 2;
    rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345679",
                        &s->chars[0].uuid);
    s->chars[0].handle = 2;
    s->chars[0].props = RB_GATT_PROP_READ | RB_GATT_PROP_WRITE |
                        RB_GATT_PROP_WRITE_NO_RSP;
    rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345680",
                        &s->chars[1].uuid);
    s->chars[1].handle = 3;
    s->chars[1].props = RB_GATT_PROP_READ;
}

const rb_ble_device_t *rb_mock_ble_catalog(size_t *count) {
    rb_once(&g_catalog_once, build_catalog);
    if (count != NULL) *count = MOCK_NDEV;
    /* Return a pointer to the first device struct; devices are laid out
     * with stride sizeof(mock_device_def_t), so callers must use
     * rb_mock_ble_device_at() instead of indexing this directly. */
    return &g_catalog[0].dev;
}

static const mock_device_def_t *mock_find(const char *address) {
    rb_once(&g_catalog_once, build_catalog);
    for (int i = 0; i < MOCK_NDEV; i++) {
        if (strcmp(g_catalog[i].dev.address, address) == 0)
            return &g_catalog[i];
    }
    return NULL;
}

static const mock_char_def_t *mock_find_char(const mock_device_def_t *dev,
                                             const rb_uuid_t *svc,
                                             const rb_uuid_t *chr) {
    for (size_t i = 0; i < dev->svc_count; i++) {
        if (!rb_uuid_equals(&dev->svcs[i].uuid, svc)) continue;
        for (size_t j = 0; j < dev->svcs[i].char_count; j++) {
            if (rb_uuid_equals(&dev->svcs[i].chars[j].uuid, chr))
                return &dev->svcs[i].chars[j];
        }
    }
    return NULL;
}

static int ci_contains(const char *haystack, const char *needle) {
    if (needle[0] == '\0') return 1;
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            j++;
        }
        if (j == nn) return 1;
    }
    return 0;
}

static int mock_filter_match(const rb_ble_device_t *dev,
                             const rb_ble_scan_options_t *opts) {
    if (opts->service_uuid_count > 0) {
        int found = 0;
        for (size_t i = 0; i < opts->service_uuid_count && !found; i++) {
            for (size_t j = 0; j < dev->service_uuid_count; j++) {
                if (rb_uuid_equals(&opts->service_uuids[i],
                                   &dev->service_uuids[j])) {
                    found = 1;
                    break;
                }
            }
        }
        if (!found) return 0;
    }
    if (opts->manufacturer_id >= 0) {
        if (!dev->has_manufacturer_data ||
            dev->manufacturer_id != (uint16_t)opts->manufacturer_id)
            return 0;
    }
    if (opts->has_min_rssi && dev->rssi_dbm < opts->min_rssi_dbm)
        return 0;
    if (!ci_contains(dev->name, opts->name_substring)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    rb_context_t *ctx;
    mock_state_t *st;
} scan_arg_t;

static void *mock_scan_thread(void *arg) {
    scan_arg_t *a = (scan_arg_t *)arg;
    rb_context_t *ctx = a->ctx;
    mock_state_t *st = a->st;
    free(a);
    uint64_t start = rb_now_ms();
    size_t count = 0;
    const rb_ble_device_t *devs = rb_mock_ble_catalog(&count);
    (void)devs;
    rb_once(&g_catalog_once, build_catalog);
    for (int i = 0; i < MOCK_NDEV; i++) {
        if (st->scan_stop || st->stopping) break;
        if (rb_op_is_cancelled(ctx, st->scan_op)) break;
        if (st->scan_opts.timeout_ms > 0 &&
            rb_now_ms() - start >= st->scan_opts.timeout_ms)
            break;
        const mock_device_def_t *def = &g_catalog[i];
        if (!mock_filter_match(&def->dev, &st->scan_opts)) continue;
        rb_ble_scan_callback_t cb = st->scan_cb;
        void *ud = st->scan_ud;
        if (cb != NULL) cb(ctx, &def->dev, ud);
        /* Small pacing so cancellation/timeout tests can interleave. */
        for (int s = 0; s < 8; s++) {
            if (st->scan_stop || st->stopping) break;
            if (rb_op_is_cancelled(ctx, st->scan_op)) break;
            rb_sleep_ms(10);
        }
    }
    rb_mutex_lock(&st->lock);
    st->scan_running = 0;
    rb_mutex_unlock(&st->lock);
    rb_op_mark_done(ctx, st->scan_op);
    return NULL;
}

static rb_status_t mock_ble_scan_start(rb_context_t *ctx,
                                       const rb_ble_scan_options_t *opts,
                                       rb_ble_scan_callback_t cb, void *ud,
                                       rb_operation_id_t op) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    if (st->scan_running) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_ALREADY_IN_PROGRESS;
    }
    st->scan_opts = *opts;
    st->scan_cb = cb;
    st->scan_ud = ud;
    st->scan_op = op;
    st->scan_stop = 0;
    st->scan_running = 1;
    scan_arg_t *a = (scan_arg_t *)malloc(sizeof(*a));
    if (a == NULL) {
        st->scan_running = 0;
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NO_MEMORY;
    }
    a->ctx = ctx;
    a->st = st;
    if (rb_thread_create(&st->scan_thread, mock_scan_thread, a) != 0) {
        free(a);
        st->scan_running = 0;
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_mutex_unlock(&st->lock);
    return RB_OK;
}

static rb_status_t mock_ble_scan_stop(rb_context_t *ctx,
                                      rb_operation_id_t op) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_thread_t th = RB_THREAD_INVALID;
    rb_mutex_lock(&st->lock);
    if (!st->scan_running || st->scan_op != op) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_FOUND;
    }
    st->scan_stop = 1;
    th = st->scan_thread;
    rb_mutex_unlock(&st->lock);
    if (!rb_thread_equal(rb_thread_self(), th)) rb_thread_join(th);
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

static mock_conn_t *mock_conn_find(mock_state_t *st, const char *address) {
    for (mock_conn_t *c = st->conns; c != NULL; c = c->next) {
        if (strcmp(c->address, address) == 0) return c;
    }
    return NULL;
}

static void mock_conn_event(rb_context_t *ctx, mock_conn_t *c,
                            rb_ble_connection_state_t state,
                            rb_status_t status, const char *message) {
    rb_ble_connection_callback_t cb = c->cb;
    void *ud = c->ud;
    rb_ble_connection_event_t ev;
    memset(&ev, 0, sizeof(ev));
    rb_copy_str(ev.device.address, sizeof(ev.device.address), c->address);
    ev.state = state;
    ev.status = status;
    rb_copy_str(ev.message, sizeof(ev.message), message);
    if (cb != NULL) cb(ctx, &ev, ud);
}

typedef struct {
    rb_context_t *ctx;
    mock_conn_t *conn;
} conn_arg_t;

static void *mock_connect_thread(void *arg) {
    conn_arg_t *a = (conn_arg_t *)arg;
    rb_context_t *ctx = a->ctx;
    mock_conn_t *c = a->conn;
    free(a);
    rb_sleep_ms(100);
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    int cancelled = rb_op_is_cancelled(ctx, c->op);
    int known = mock_find(c->address) != NULL;
    int stopping = st->stopping;
    rb_mutex_unlock(&st->lock);
    rb_operation_id_t op = c->op;
    if (cancelled || stopping) {
        mock_conn_event(ctx, c, RB_BLE_FAILED, RB_ERROR_CANCELLED,
                        "connect cancelled");
        rb_mutex_lock(&st->lock);
        /* Remove the failed entry. */
        mock_conn_t **pp = &st->conns;
        while (*pp != NULL) {
            if (*pp == c) {
                *pp = c->next;
                break;
            }
            pp = &(*pp)->next;
        }
        rb_mutex_unlock(&st->lock);
        free(c);
        rb_op_mark_done(ctx, op);
        return NULL;
    }
    if (!known) {
        mock_conn_event(ctx, c, RB_BLE_FAILED, RB_ERROR_NOT_FOUND,
                        "device not found");
        rb_mutex_lock(&st->lock);
        mock_conn_t **pp = &st->conns;
        while (*pp != NULL) {
            if (*pp == c) {
                *pp = c->next;
                break;
            }
            pp = &(*pp)->next;
        }
        rb_mutex_unlock(&st->lock);
        free(c);
        rb_op_mark_done(ctx, op);
        return NULL;
    }
    rb_mutex_lock(&st->lock);
    c->state = RB_BLE_CONNECTED;
    rb_mutex_unlock(&st->lock);
    mock_conn_event(ctx, c, RB_BLE_CONNECTED, RB_OK, "connected");
    rb_op_mark_done(ctx, c->op);
    return NULL;
}

static rb_status_t mock_ble_connect(rb_context_t *ctx,
                                    const rb_device_id_t *device,
                                    const rb_ble_connect_options_t *opts,
                                    rb_ble_connection_callback_t cb,
                                    void *ud, rb_operation_id_t op) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *existing = mock_conn_find(st, device->address);
    if (existing != NULL &&
        (existing->state == RB_BLE_CONNECTED ||
         existing->state == RB_BLE_CONNECTING)) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_ALREADY_IN_PROGRESS;
    }
    if (existing != NULL) {
        /* Drop a stale FAILED/DISCONNECTED entry so reconnect works.
         * Join a running reconnect worker first — never free an entry
         * a thread still touches. */
        int rec_running = existing->reconnect_running;
        rb_thread_t rec_thread = existing->reconnect_thread;
        if (rec_running) existing->reconnect_stop = 1;
        mock_conn_t **pp = &st->conns;
        while (*pp != NULL) {
            if (*pp == existing) {
                *pp = existing->next;
                break;
            }
            pp = &(*pp)->next;
        }
        rb_mutex_unlock(&st->lock);
        if (rec_running && !rb_thread_equal(rb_thread_self(), rec_thread))
            rb_thread_join(rec_thread);
        free(existing);
        rb_mutex_lock(&st->lock);
    }
    mock_conn_t *c = (mock_conn_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NO_MEMORY;
    }
    rb_copy_str(c->address, sizeof(c->address), device->address);
    c->state = RB_BLE_CONNECTING;
    c->cb = cb;
    c->ud = ud;
    c->op = op;
    c->auto_reconnect = opts != NULL && opts->auto_reconnect;
    c->max_reconnect_attempts =
        (opts != NULL && opts->max_reconnect_attempts > 0)
            ? opts->max_reconnect_attempts
            : st->policy.max_attempts;
    c->reconnect_delay_ms =
        (opts != NULL && opts->reconnect_delay_ms > 0)
            ? opts->reconnect_delay_ms
            : st->policy.initial_delay_ms;
    c->next = st->conns;
    st->conns = c;
    conn_arg_t *a = (conn_arg_t *)malloc(sizeof(*a));
    if (a == NULL) {
        st->conns = c->next;
        free(c);
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NO_MEMORY;
    }
    a->ctx = ctx;
    a->conn = c;
    rb_thread_t th;
    if (rb_thread_create(&th, mock_connect_thread, a) != 0) {
        free(a);
        st->conns = c->next;
        free(c);
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    rb_mutex_unlock(&st->lock);
    mock_conn_event(ctx, c, RB_BLE_CONNECTING, RB_OK, "connecting");
    return RB_OK;
}

static void mock_stop_subs_for(mock_state_t *st, const char *address) {
    /* Collect threads to join without holding the lock. */
    mock_sub_t *doomed[64];
    size_t n = 0;
    rb_mutex_lock(&st->lock);
    mock_sub_t **pp = &st->subs;
    while (*pp != NULL) {
        mock_sub_t *s = *pp;
        if (address == NULL || strcmp(s->address, address) == 0) {
            s->stop = 1;
            if (n < 64) doomed[n++] = s;
            *pp = s->next;
        } else {
            pp = &s->next;
        }
    }
    rb_mutex_unlock(&st->lock);
    for (size_t i = 0; i < n; i++) {
        if (doomed[i]->running &&
            !rb_thread_equal(rb_thread_self(), doomed[i]->thread))
            rb_thread_join(doomed[i]->thread);
        free(doomed[i]);
    }
}

static rb_status_t mock_ble_disconnect(rb_context_t *ctx,
                                       const rb_device_id_t *device) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    c->state = RB_BLE_DISCONNECTED;
    int rec_running = c->reconnect_running;
    rb_thread_t rec_thread = c->reconnect_thread;
    if (rec_running) c->reconnect_stop = 1;
    mock_conn_t **pp = &st->conns;
    while (*pp != NULL) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
        pp = &(*pp)->next;
    }
    rb_mutex_unlock(&st->lock);
    if (rec_running && !rb_thread_equal(rb_thread_self(), rec_thread))
        rb_thread_join(rec_thread);
    mock_stop_subs_for(st, device->address);
    rb_emit_event(ctx, RB_EVENT_BACKEND_STATE, RB_OK,
                  "BLE device disconnected");
    free(c);
    return RB_OK;
}

/* Reconnect worker: delay, CONNECTING attempt note, CONNECTED. */
static void *mock_reconnect_thread(void *arg) {
    conn_arg_t *a = (conn_arg_t *)arg;
    rb_context_t *ctx = a->ctx;
    mock_conn_t *c = a->conn;
    free(a);
    mock_state_t *st = rb_mock_state(ctx);
    uint32_t delay = c->reconnect_delay_ms;
    uint32_t max_attempts = c->max_reconnect_attempts;
    for (uint32_t attempt = 1;; attempt++) {
        for (uint32_t slept = 0; slept < delay; slept += 20) {
            if (c->reconnect_stop || st->stopping) goto done;
            rb_sleep_ms(20);
        }
        if (c->reconnect_stop || st->stopping) goto done;
        char msg[128];
        if (max_attempts > 0)
            snprintf(msg, sizeof(msg), "reconnect attempt %u/%u", attempt,
                     max_attempts);
        else
            snprintf(msg, sizeof(msg), "reconnect attempt %u", attempt);
        mock_conn_event(ctx, c, RB_BLE_CONNECTING, RB_OK, msg);
        rb_sleep_ms(100);
        if (c->reconnect_stop || st->stopping) goto done;
        rb_mutex_lock(&st->lock);
        c->state = RB_BLE_CONNECTED;
        rb_mutex_unlock(&st->lock);
        mock_conn_event(ctx, c, RB_BLE_CONNECTED, RB_OK,
                        "reconnected");
        break; /* Mock radios always succeed on the first attempt. */
    }
done:
    rb_mutex_lock(&st->lock);
    c->reconnect_running = 0;
    rb_mutex_unlock(&st->lock);
    return NULL;
}

static rb_status_t mock_trigger_disconnect(rb_context_t *ctx,
                                           const rb_device_id_t *device) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    c->state = RB_BLE_DISCONNECTED;
    int want_reconnect = c->auto_reconnect && st->policy.enabled;
    rb_mutex_unlock(&st->lock);
    mock_conn_event(ctx, c, RB_BLE_DISCONNECTED, RB_OK,
                    "simulated radio drop");
    if (want_reconnect) {
        rb_mutex_lock(&st->lock);
        c->reconnect_stop = 0;
        c->reconnect_running = 1;
        rb_mutex_unlock(&st->lock);
        conn_arg_t *a = (conn_arg_t *)malloc(sizeof(*a));
        if (a == NULL) return RB_ERROR_NO_MEMORY;
        a->ctx = ctx;
        a->conn = c;
        if (rb_thread_create(&c->reconnect_thread,
                           mock_reconnect_thread, a) != 0) {
            free(a);
            rb_mutex_lock(&st->lock);
            c->reconnect_running = 0;
            rb_mutex_unlock(&st->lock);
            return RB_ERROR_INTERNAL;
        }
    }
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* GATT                                                                */
/* ------------------------------------------------------------------ */

/* Readable characteristic values. The echo characteristic reads back
 * the last written value; everything else returns a fixed synthetic
 * payload. */
static rb_status_t mock_char_value(mock_state_t *st,
                                   const mock_device_def_t *dev,
                                   const mock_char_def_t *chr,
                                   uint8_t *out, size_t *out_len) {
    if ((chr->props & RB_GATT_PROP_READ) == 0)
        return RB_ERROR_NOT_SUPPORTED;
    /* Custom echo characteristic (demo tag). */
    rb_uuid_t echo_uuid;
    rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345679",
                        &echo_uuid);
    if (rb_uuid_equals(&chr->uuid, &echo_uuid)) {
        memcpy(out, st->echo_value, st->echo_len);
        *out_len = st->echo_len;
        return RB_OK;
    }
    /* 16-bit standard characteristics. */
    uint16_t short_uuid =
        (uint16_t)((chr->uuid.bytes[2] << 8) | chr->uuid.bytes[3]);
    switch (short_uuid) {
        case 0x2A38: /* Body Sensor Location: chest. */
            out[0] = 0x01;
            *out_len = 1;
            return RB_OK;
        case 0x2A19: /* Battery Level: 87%. */
            out[0] = 87;
            *out_len = 1;
            return RB_OK;
        case 0x2A1C: { /* Temperature Measurement: 36.5 C as LE float. */
            float t = 36.5f;
            memcpy(out, &t, 4);
            *out_len = 4;
            return RB_OK;
        }
        case 0x2A1D: /* Temperature Type: body. */
            out[0] = 0x02;
            *out_len = 1;
            return RB_OK;
        default:
            break;
    }
    /* Custom fixed characteristic (demo tag). */
    out[0] = 0xDE;
    out[1] = 0xAD;
    out[2] = 0xBE;
    out[3] = 0xEF;
    *out_len = 4;
    (void)dev;
    return RB_OK;
}

static rb_status_t mock_ble_discover(rb_context_t *ctx,
                                     const rb_device_id_t *device,
                                     rb_ble_service_t **out_services,
                                     size_t *out_count) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    rb_mutex_unlock(&st->lock);
    const mock_device_def_t *def = mock_find(device->address);
    if (def == NULL) return RB_ERROR_NOT_FOUND;
    rb_ble_service_t *svcs =
        (rb_ble_service_t *)calloc(def->svc_count, sizeof(*svcs));
    if (svcs == NULL) return RB_ERROR_NO_MEMORY;
    for (size_t i = 0; i < def->svc_count; i++) {
        svcs[i].uuid = def->svcs[i].uuid;
        svcs[i].handle = def->svcs[i].handle;
        svcs[i].characteristic_count = def->svcs[i].char_count;
        for (size_t j = 0; j < def->svcs[i].char_count; j++) {
            rb_ble_characteristic_t *ch = &svcs[i].characteristics[j];
            ch->uuid = def->svcs[i].chars[j].uuid;
            ch->handle = def->svcs[i].chars[j].handle;
            ch->properties = def->svcs[i].chars[j].props;
            ch->descriptor_count = def->svcs[i].chars[j].desc_count;
            for (size_t k = 0; k < def->svcs[i].chars[j].desc_count; k++) {
                ch->descriptors[k].uuid = def->svcs[i].chars[j].descs[k];
                ch->descriptors[k].handle =
                    (uint16_t)(def->svcs[i].chars[j].handle + 1 + k);
            }
        }
    }
    *out_services = svcs;
    *out_count = def->svc_count;
    return RB_OK;
}

static rb_status_t mock_ble_read(rb_context_t *ctx,
                                 const rb_device_id_t *device,
                                 const rb_uuid_t *service_uuid,
                                 const rb_uuid_t *characteristic_uuid,
                                 uint8_t *out_value, size_t *out_len) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    rb_mutex_unlock(&st->lock);
    const mock_device_def_t *def = mock_find(device->address);
    if (def == NULL) return RB_ERROR_NOT_FOUND;
    const mock_char_def_t *chr =
        mock_find_char(def, service_uuid, characteristic_uuid);
    if (chr == NULL) return RB_ERROR_NOT_FOUND;
    uint8_t buf[RB_BLE_MAX_VALUE_LEN];
    size_t len = 0;
    rb_status_t r = mock_char_value(st, def, chr, buf, &len);
    if (r != RB_OK) return r;
    memcpy(out_value, buf, len);
    *out_len = len;
    return RB_OK;
}

static rb_status_t mock_ble_write(rb_context_t *ctx,
                                  const rb_device_id_t *device,
                                  const rb_uuid_t *service_uuid,
                                  const rb_uuid_t *characteristic_uuid,
                                  const uint8_t *value, size_t value_len,
                                  int with_response) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    rb_mutex_unlock(&st->lock);
    const mock_device_def_t *def = mock_find(device->address);
    if (def == NULL) return RB_ERROR_NOT_FOUND;
    const mock_char_def_t *chr =
        mock_find_char(def, service_uuid, characteristic_uuid);
    if (chr == NULL) return RB_ERROR_NOT_FOUND;
    uint32_t need = with_response ? RB_GATT_PROP_WRITE
                                  : RB_GATT_PROP_WRITE_NO_RSP;
    if ((chr->props & need) == 0) return RB_ERROR_NOT_SUPPORTED;
    /* Only the demo-tag echo characteristic is writable in the catalog;
     * other writable characteristics would store here in the same way. */
    rb_mutex_lock(&st->lock);
    size_t n = value_len > RB_BLE_MAX_VALUE_LEN ? RB_BLE_MAX_VALUE_LEN
                                                : value_len;
    if (n > 0) memcpy(st->echo_value, value, n);
    st->echo_len = n;
    rb_mutex_unlock(&st->lock);
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Notifications                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    rb_context_t *ctx;
    mock_sub_t *sub;
} sub_arg_t;

static void *mock_notify_thread(void *arg) {
    sub_arg_t *a = (sub_arg_t *)arg;
    rb_context_t *ctx = a->ctx;
    mock_sub_t *sub = a->sub;
    free(a);
    unsigned int counter = 0;
    while (!sub->stop) {
        rb_sleep_ms(300);
        if (sub->stop) break;
        counter++;
        rb_ble_notification_t n;
        memset(&n, 0, sizeof(n));
        rb_copy_str(n.device.address, sizeof(n.device.address),
                    sub->address);
        n.service_uuid = sub->service_uuid;
        n.characteristic_uuid = sub->characteristic_uuid;
        /* Synthetic payload per characteristic family. */
        uint16_t short_uuid = (uint16_t)((sub->characteristic_uuid.bytes[2] << 8) |
                                         sub->characteristic_uuid.bytes[3]);
        switch (short_uuid) {
            case 0x2A37: /* Heart Rate Measurement. */
                n.data[0] = 0x00;
                n.data[1] = (uint8_t)(70 + (counter % 30));
                n.data_len = 2;
                break;
            case 0x2A19: /* Battery Level. */
                n.data[0] = 87;
                n.data_len = 1;
                break;
            case 0x2A1C: { /* Temperature Measurement. */
                float t = 36.5f + (float)(counter % 10) * 0.1f;
                memcpy(n.data, &t, 4);
                n.data_len = 4;
                break;
            }
            default:
                n.data[0] = (uint8_t)counter;
                n.data_len = 1;
                break;
        }
        rb_ble_notify_callback_t cb = sub->cb;
        void *ud = sub->ud;
        if (cb != NULL) cb(ctx, &n, ud);
    }
    return NULL;
}

static rb_status_t mock_ble_subscribe(rb_context_t *ctx,
                                      const rb_device_id_t *device,
                                      const rb_uuid_t *service_uuid,
                                      const rb_uuid_t *characteristic_uuid,
                                      rb_ble_notify_callback_t cb,
                                      void *ud) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    rb_mutex_unlock(&st->lock);
    const mock_device_def_t *def = mock_find(device->address);
    if (def == NULL) return RB_ERROR_NOT_FOUND;
    const mock_char_def_t *chr =
        mock_find_char(def, service_uuid, characteristic_uuid);
    if (chr == NULL) return RB_ERROR_NOT_FOUND;
    if ((chr->props & (RB_GATT_PROP_NOTIFY | RB_GATT_PROP_INDICATE)) == 0)
        return RB_ERROR_NOT_SUPPORTED;
    mock_sub_t *sub = (mock_sub_t *)calloc(1, sizeof(*sub));
    if (sub == NULL) return RB_ERROR_NO_MEMORY;
    rb_copy_str(sub->address, sizeof(sub->address), device->address);
    sub->service_uuid = *service_uuid;
    sub->characteristic_uuid = *characteristic_uuid;
    sub->cb = cb;
    sub->ud = ud;
    sub_arg_t *a = (sub_arg_t *)malloc(sizeof(*a));
    if (a == NULL) {
        free(sub);
        return RB_ERROR_NO_MEMORY;
    }
    a->ctx = ctx;
    a->sub = sub;
    rb_mutex_lock(&st->lock);
    sub->next = st->subs;
    st->subs = sub;
    rb_mutex_unlock(&st->lock);
    if (rb_thread_create(&sub->thread, mock_notify_thread, a) != 0) {
        free(a);
        rb_mutex_lock(&st->lock);
        mock_sub_t **pp = &st->subs;
        while (*pp != NULL) {
            if (*pp == sub) {
                *pp = sub->next;
                break;
            }
            pp = &(*pp)->next;
        }
        rb_mutex_unlock(&st->lock);
        free(sub);
        return RB_ERROR_INTERNAL;
    }
    rb_mutex_lock(&st->lock);
    sub->running = 1;
    rb_mutex_unlock(&st->lock);
    return RB_OK;
}

static rb_status_t mock_ble_unsubscribe(rb_context_t *ctx,
                                        const rb_device_id_t *device,
                                        const rb_uuid_t *service_uuid,
                                        const rb_uuid_t *characteristic_uuid) {
    mock_state_t *st = rb_mock_state(ctx);
    mock_sub_t *target = NULL;
    rb_mutex_lock(&st->lock);
    mock_sub_t **pp = &st->subs;
    while (*pp != NULL) {
        mock_sub_t *s = *pp;
        if (strcmp(s->address, device->address) == 0 &&
            rb_uuid_equals(&s->service_uuid, service_uuid) &&
            rb_uuid_equals(&s->characteristic_uuid,
                           characteristic_uuid)) {
            s->stop = 1;
            target = s;
            *pp = s->next;
            break;
        }
        pp = &s->next;
    }
    rb_mutex_unlock(&st->lock);
    if (target == NULL) return RB_ERROR_NOT_FOUND;
    if (target->running &&
        !rb_thread_equal(rb_thread_self(), target->thread))
        rb_thread_join(target->thread);
    free(target);
    return RB_OK;
}

static rb_status_t mock_ble_get_rssi(rb_context_t *ctx,
                                     const rb_device_id_t *device,
                                     int8_t *out_rssi_dbm) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    mock_conn_t *c = mock_conn_find(st, device->address);
    if (c == NULL || c->state != RB_BLE_CONNECTED) {
        rb_mutex_unlock(&st->lock);
        return RB_ERROR_NOT_CONNECTED;
    }
    rb_mutex_unlock(&st->lock);
    const mock_device_def_t *def = mock_find(device->address);
    if (def == NULL) return RB_ERROR_NOT_FOUND;
    *out_rssi_dbm = def->dev.rssi_dbm;
    return RB_OK;
}

static rb_status_t mock_ble_set_reconnect_policy(
    rb_context_t *ctx, const rb_reconnect_policy_t *policy) {
    mock_state_t *st = rb_mock_state(ctx);
    rb_mutex_lock(&st->lock);
    st->policy = *policy;
    rb_mutex_unlock(&st->lock);
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Probe / lifecycle / permissions / ops table                         */
/* ------------------------------------------------------------------ */

rb_status_t rb_mock_probe(rb_capabilities_t *caps) {
    if (caps == NULL) return RB_ERROR_INVALID_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
    rb_copy_str(caps->backend_name, sizeof(caps->backend_name), "mock");
    rb_copy_str(caps->backend_version, sizeof(caps->backend_version),
                "0.1.0");
    rb_copy_str(caps->os_name, sizeof(caps->os_name), "mock");
    caps->ble.scan = RB_CAP_AVAILABLE;
    caps->ble.connect = RB_CAP_AVAILABLE;
    caps->ble.gatt = RB_CAP_AVAILABLE;
    caps->ble.notify = RB_CAP_AVAILABLE;
    caps->ble.pairing = RB_CAP_NOT_SUPPORTED;
    caps->ble.rssi = RB_CAP_AVAILABLE;
    caps->ble.reconnect = RB_CAP_AVAILABLE;
    caps->wifi.status = RB_CAP_AVAILABLE;
    caps->wifi.scan = RB_CAP_AVAILABLE;
    caps->wifi.connect = RB_CAP_AVAILABLE;
    caps->wifi.quality = RB_CAP_AVAILABLE;
    caps->wifi.saved_networks = RB_CAP_NOT_SUPPORTED;
    rb_copy_str(caps->detail, sizeof(caps->detail),
                "mock backend: deterministic fake radios, no hardware "
                "required; pairing and saved-network APIs not in the "
                "Phase 1 surface");
    return RB_OK;
}

static rb_status_t mock_create(rb_context_t *ctx) {
    mock_state_t *st = (mock_state_t *)calloc(1, sizeof(*st));
    if (st == NULL) return RB_ERROR_NO_MEMORY;
    rb_mutex_init(&st->lock);
    st->ctx = ctx;
    st->policy.enabled = 1;
    st->policy.max_attempts = 3;
    st->policy.initial_delay_ms = 500;
    st->policy.backoff_factor_x100 = 200;
    memcpy(st->echo_value, "RB-OK", 5);
    st->echo_len = 5;
    st->wifi_state = RB_WIFI_DISCONNECTED;
    st->wifi_diag = RB_WIFI_DIAG_DISCONNECTED;
    ctx->backend_state = st;
    return RB_OK;
}

static void mock_destroy(rb_context_t *ctx) {
    mock_state_t *st = rb_mock_state(ctx);
    if (st == NULL) return;
    st->stopping = 1;
    /* Stop scans first (joins their threads). */
    rb_mutex_lock(&st->lock);
    int scan_running = st->scan_running;
    rb_operation_id_t scan_op = st->scan_op;
    rb_mutex_unlock(&st->lock);
    if (scan_running) {
        rb_mutex_lock(&st->lock);
        st->scan_stop = 1;
        rb_mutex_unlock(&st->lock);
        rb_thread_join(st->scan_thread);
        rb_op_mark_done(ctx, scan_op);
    }
    rb_mock_wifi_stop_all(st);
    /* Stop subscriptions and reconnect workers, then free entries. */
    mock_stop_subs_for(st, NULL);
    rb_mutex_lock(&st->lock);
    mock_conn_t *conns = st->conns;
    st->conns = NULL;
    for (mock_conn_t *c = conns; c != NULL; c = c->next) {
        if (c->reconnect_running) c->reconnect_stop = 1;
    }
    rb_mutex_unlock(&st->lock);
    for (mock_conn_t *c = conns; c != NULL;) {
        mock_conn_t *next = c->next;
        if (c->reconnect_running) rb_thread_join(c->reconnect_thread);
        free(c);
        c = next;
    }
    rb_mutex_destroy(&st->lock);
    free(st);
    ctx->backend_state = NULL;
}

static rb_status_t mock_required_permissions(rb_operation_t op,
                                             rb_permission_mask_t *mask) {
    if (mask == NULL) return RB_ERROR_INVALID_ARGUMENT;
    switch (op) {
        case RB_OP_BLE_SCAN:
            *mask = RB_PERM_BLUETOOTH_SCAN | RB_PERM_LOCATION;
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

const rb_backend_ops_t rb_mock_backend_ops = {
    .name = "mock",
    .version = "0.1.0",
    .probe = rb_mock_probe,
    .create = mock_create,
    .destroy = mock_destroy,
    .ble_scan_start = mock_ble_scan_start,
    .ble_scan_stop = mock_ble_scan_stop,
    .ble_connect = mock_ble_connect,
    .ble_disconnect = mock_ble_disconnect,
    .ble_discover = mock_ble_discover,
    .ble_read = mock_ble_read,
    .ble_write = mock_ble_write,
    .ble_subscribe = mock_ble_subscribe,
    .ble_unsubscribe = mock_ble_unsubscribe,
    .ble_get_rssi = mock_ble_get_rssi,
    .ble_set_reconnect_policy = mock_ble_set_reconnect_policy,
    .wifi_get_status = rb_mock_wifi_get_status,
    .wifi_scan_start = rb_mock_wifi_scan_start,
    .wifi_scan_stop = rb_mock_wifi_scan_stop,
    .wifi_connect = rb_mock_wifi_connect,
    .wifi_disconnect = rb_mock_wifi_disconnect,
    .wifi_measure_quality = rb_mock_wifi_measure_quality,
    .required_permissions = mock_required_permissions,
    .mock_trigger_disconnect = mock_trigger_disconnect,
    .mock_trigger_wifi_change = rb_mock_wifi_trigger_change,
};
