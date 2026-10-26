/* Cross-platform conformance suite (Phase 5).
 *
 * Runs the same scripted sequence against one backend (argv[1]:
 * "mock" or "linux") and asserts equivalent documented behavior:
 * valid addresses, connect -> connected, non-empty discovery, read
 * returns bytes, consistent error mapping. It deliberately does NOT
 * compare raw counts: the mock catalog has 3 devices / 3 networks,
 * the fake-bus catalog has 2 / 2, so counts legitimately differ.
 *
 * Documented divergences asserted per backend:
 *  - Wi-Fi scan signal: mock reports real dBm (< 0); Linux reports 0
 *    ("not reported") because NetworkManager exposes Strength percent,
 *    not dBm, and the backend never fabricates a percent->dBm mapping.
 *  - BLE reconnect capability: AVAILABLE on mock, UNAVAILABLE on Linux
 *    (policy stored, auto-reconnect not attempted).
 *
 * The Linux run needs RB_DBUS_ADDRESS pointing at a bus running the
 * fake BlueZ/NetworkManager service (see tests/run_conformance.sh).
 * Without a bus, the Linux run fails at init — by design, not skipped
 * silently: the wrapper script only runs it when the fake bus is up.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rb_internal.h"
#include "radiobridge/rb.h"
#include "radiobridge/rb_bluetooth.h"
#include "radiobridge/rb_wifi.h"

static int g_failures = 0;

#define CONF_CHECK(cond, ...)                                     \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "CONFORMANCE FAIL: ");                \
            fprintf(stderr, __VA_ARGS__);                         \
            fprintf(stderr, "\n");                                \
            g_failures++;                                         \
        }                                                         \
    } while (0)

typedef struct {
    rb_ble_device_t devices[16];
    size_t count;
    rb_mutex_t mu;
} scan_collector_t;

static void scan_cb(rb_context_t *ctx, const rb_ble_device_t *device,
                    void *ud) {
    (void)ctx;
    scan_collector_t *col = (scan_collector_t *)ud;
    rb_mutex_lock(&col->mu);
    if (col->count < 16) col->devices[col->count++] = *device;
    rb_mutex_unlock(&col->mu);
}

typedef struct {
    rb_wifi_network_t networks[16];
    size_t count;
    rb_mutex_t mu;
} wifi_collector_t;

static void wifi_cb(rb_context_t *ctx, const rb_wifi_network_t *network,
                    void *ud) {
    (void)ctx;
    wifi_collector_t *col = (wifi_collector_t *)ud;
    rb_mutex_lock(&col->mu);
    if (col->count < 16) col->networks[col->count++] = *network;
    rb_mutex_unlock(&col->mu);
}

typedef struct {
    rb_ble_connection_state_t state;
    rb_status_t status;
    int seen;
    rb_mutex_t mu;
} conn_collector_t;

static void conn_cb(rb_context_t *ctx,
                    const rb_ble_connection_event_t *ev, void *ud) {
    (void)ctx;
    conn_collector_t *col = (conn_collector_t *)ud;
    rb_mutex_lock(&col->mu);
    col->state = ev->state;
    col->status = ev->status;
    if (ev->state == RB_BLE_CONNECTED || ev->state == RB_BLE_FAILED)
        col->seen = 1;
    rb_mutex_unlock(&col->mu);
}

static int wait_conn(conn_collector_t *col, uint32_t timeout_ms) {
    uint64_t deadline = rb_now_ms() + timeout_ms;
    for (;;) {
        rb_mutex_lock(&col->mu);
        int seen = col->seen;
        rb_mutex_unlock(&col->mu);
        if (seen) return 1;
        if (rb_now_ms() >= deadline) return 0;
        rb_sleep_ms(20);
    }
}

static int address_shape_ok(const char *addr) {
    if (strlen(addr) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (addr[i] != ':') return 0;
        } else if (!((addr[i] >= '0' && addr[i] <= '9') ||
                     (addr[i] >= 'A' && addr[i] <= 'F') ||
                     (addr[i] >= 'a' && addr[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_conformance <mock|linux>\n");
        return 2;
    }
    int is_linux = strcmp(argv[1], "linux") == 0;
    int is_mock = strcmp(argv[1], "mock") == 0;
    if (!is_linux && !is_mock) {
        fprintf(stderr, "unknown backend: %s\n", argv[1]);
        return 2;
    }

    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = is_linux ? RB_BACKEND_LINUX : RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    rb_status_t st = rb_initialize(&cfg, &ctx);
    if (st != RB_OK) {
        fprintf(stderr, "CONFORMANCE FAIL: init %s -> %d\n", argv[1],
                (int)st);
        return 1;
    }

    rb_capabilities_t caps;
    memset(&caps, 0, sizeof(caps));
    CONF_CHECK(rb_get_capabilities(ctx, &caps) == RB_OK,
               "get_capabilities failed");
    printf("CONFORMANCE %s: backend=%s\n", argv[1], caps.backend_name);
    /* Equivalent capability shape on both backends. */
    CONF_CHECK(caps.ble.scan == RB_CAP_AVAILABLE, "ble_scan cap");
    CONF_CHECK(caps.ble.connect == RB_CAP_AVAILABLE,
               "ble_connect cap");
    CONF_CHECK(caps.ble.gatt == RB_CAP_AVAILABLE, "ble_gatt cap");
    CONF_CHECK(caps.ble.notify == RB_CAP_AVAILABLE, "ble_notify cap");
    CONF_CHECK(caps.ble.rssi == RB_CAP_AVAILABLE, "ble_rssi cap");
    CONF_CHECK(caps.wifi.status == RB_CAP_AVAILABLE,
               "wifi_status cap");
    CONF_CHECK(caps.wifi.scan == RB_CAP_AVAILABLE, "wifi_scan cap");
    CONF_CHECK(caps.wifi.connect == RB_CAP_AVAILABLE,
               "wifi_connect cap");
    /* Documented divergence: reconnect. */
    if (is_mock)
        CONF_CHECK(caps.ble.reconnect == RB_CAP_AVAILABLE,
                   "mock ble_reconnect cap");
    else
        CONF_CHECK(caps.ble.reconnect == RB_CAP_UNAVAILABLE,
                   "linux ble_reconnect cap");

    /* BLE scan. */
    scan_collector_t scol;
    memset(&scol, 0, sizeof(scol));
    rb_mutex_init(&scol.mu);
    rb_ble_scan_options_t sopts;
    memset(&sopts, 0, sizeof(sopts));
    sopts.timeout_ms = 8000;
    sopts.manufacturer_id = -1;
    rb_operation_id_t scan_op = 0;
    st = rb_ble_scan_start(ctx, &sopts, scan_cb, &scol, &scan_op);
    CONF_CHECK(st == RB_OK, "ble_scan_start -> %d", (int)st);
    rb_sleep_ms(10000);
    rb_ble_scan_stop(ctx, scan_op); /* already finished: NOT_FOUND ok */
    rb_mutex_lock(&scol.mu);
    size_t ndev = scol.count;
    rb_ble_device_t first;
    if (ndev > 0) first = scol.devices[0];
    rb_mutex_unlock(&scol.mu);
    CONF_CHECK(ndev >= 1, "ble scan found 0 devices");
    printf("CONFORMANCE %s: ble scan found %zu device(s)\n", argv[1],
           ndev);
    rb_mutex_lock(&scol.mu);
    for (size_t i = 0; i < scol.count; i++) {
        CONF_CHECK(address_shape_ok(scol.devices[i].address),
                   "device %zu address shape: %s", i,
                   scol.devices[i].address);
        CONF_CHECK(scol.devices[i].rssi_dbm < 0,
                   "device %zu rssi not negative: %d", i,
                   (int)scol.devices[i].rssi_dbm);
    }
    rb_mutex_unlock(&scol.mu);

    if (ndev > 0) {
        /* Connect -> discover -> read. */
        conn_collector_t ccol;
        memset(&ccol, 0, sizeof(ccol));
        rb_mutex_init(&ccol.mu);
        rb_device_id_t dev;
        rb_copy_str(dev.address, sizeof(dev.address), first.address);
        rb_operation_id_t conn_op = 0;
        st = rb_ble_connect(ctx, &dev, NULL, conn_cb, &ccol,
                            &conn_op);
        CONF_CHECK(st == RB_OK, "ble_connect -> %d", (int)st);
        CONF_CHECK(wait_conn(&ccol, 10000),
                   "connect timed out for %s", first.address);
        rb_mutex_lock(&ccol.mu);
        rb_ble_connection_state_t cstate = ccol.state;
        rb_status_t cstatus = ccol.status;
        rb_mutex_unlock(&ccol.mu);
        CONF_CHECK(cstate == RB_BLE_CONNECTED,
                   "connect state %d status %d", (int)cstate,
                   (int)cstatus);

        rb_ble_service_t *services = NULL;
        size_t scount = 0;
        st = rb_ble_discover(ctx, &dev, &services, &scount);
        CONF_CHECK(st == RB_OK, "discover -> %d", (int)st);
        CONF_CHECK(scount >= 1, "discover found 0 services");
        int read_ok = 0;
        if (st == RB_OK && scount >= 1) {
            for (size_t i = 0; i < scount; i++) {
                CONF_CHECK(services[i].characteristic_count >= 1,
                           "service %zu has 0 characteristics", i);
                for (size_t j = 0;
                     j < services[i].characteristic_count; j++) {
                    CONF_CHECK(services[i].characteristics[j].properties !=
                                   0,
                               "char %zu/%zu has 0 properties", i, j);
                    if (!read_ok &&
                        (services[i].characteristics[j].properties &
                         RB_GATT_PROP_READ)) {
                        uint8_t value[RB_BLE_MAX_VALUE_LEN];
                        size_t vlen = 0;
                        st = rb_ble_read(
                            ctx, &dev, &services[i].uuid,
                            &services[i].characteristics[j].uuid,
                            value, &vlen);
                        CONF_CHECK(st == RB_OK, "read -> %d",
                                   (int)st);
                        CONF_CHECK(vlen > 0, "read returned 0 bytes");
                        if (st == RB_OK && vlen > 0) read_ok = 1;
                    }
                }
            }
            rb_free(services);
        }
        CONF_CHECK(read_ok, "no readable characteristic returned bytes");

        st = rb_ble_disconnect(ctx, &dev);
        CONF_CHECK(st == RB_OK, "disconnect -> %d", (int)st);

        /* Error mapping: read while disconnected -> NOT_CONNECTED. */
        if (scount >= 1) {
            /* Re-discover is impossible while disconnected, so reuse
             * the first service/char UUIDs captured above is not
             * possible after free; instead expect NOT_CONNECTED from
             * a discover attempt, which is the same mapping. */
            rb_ble_service_t *tmp = NULL;
            size_t tcount = 0;
            st = rb_ble_discover(ctx, &dev, &tmp, &tcount);
            CONF_CHECK(st == RB_ERROR_NOT_CONNECTED,
                       "discover while disconnected -> %d", (int)st);
            if (st == RB_OK && tmp != NULL) rb_free(tmp);
        }
        rb_mutex_destroy(&ccol.mu);
    }

    /* Error mapping: unknown address -> FAILED/NOT_FOUND async. */
    {
        conn_collector_t ccol;
        memset(&ccol, 0, sizeof(ccol));
        rb_mutex_init(&ccol.mu);
        rb_device_id_t dev;
        rb_copy_str(dev.address, sizeof(dev.address),
                    "AA:BB:CC:FF:FF:FF");
        rb_operation_id_t conn_op = 0;
        st = rb_ble_connect(ctx, &dev, NULL, conn_cb, &ccol,
                            &conn_op);
        CONF_CHECK(st == RB_OK, "unknown connect -> %d", (int)st);
        CONF_CHECK(wait_conn(&ccol, 10000),
                   "unknown connect timed out");
        rb_mutex_lock(&ccol.mu);
        rb_ble_connection_state_t cstate = ccol.state;
        rb_status_t cstatus = ccol.status;
        rb_mutex_unlock(&ccol.mu);
        CONF_CHECK(cstate == RB_BLE_FAILED,
                   "unknown connect state %d", (int)cstate);
        CONF_CHECK(cstatus == RB_ERROR_NOT_FOUND,
                   "unknown connect status %d", (int)cstatus);
        rb_mutex_destroy(&ccol.mu);
    }

    /* Wi-Fi status shape. */
    {
        rb_wifi_status_t ws;
        memset(&ws, 0, sizeof(ws));
        st = rb_wifi_get_status(ctx, &ws);
        CONF_CHECK(st == RB_OK, "wifi status -> %d", (int)st);
        CONF_CHECK(ws.state <= RB_WIFI_FAILED, "wifi state %d",
                   (int)ws.state);
        if (ws.state == RB_WIFI_CONNECTED)
            CONF_CHECK(ws.ssid[0] != '\0',
                       "connected with empty ssid");
    }

    /* Wi-Fi scan. */
    wifi_collector_t wcol;
    memset(&wcol, 0, sizeof(wcol));
    rb_mutex_init(&wcol.mu);
    rb_wifi_scan_options_t wopts;
    memset(&wopts, 0, sizeof(wopts));
    wopts.timeout_ms = 8000;
    rb_operation_id_t wop = 0;
    st = rb_wifi_scan_start(ctx, &wopts, wifi_cb, &wcol, &wop);
    CONF_CHECK(st == RB_OK, "wifi scan -> %d", (int)st);
    rb_sleep_ms(10000);
    rb_wifi_scan_stop(ctx, wop);
    rb_mutex_lock(&wcol.mu);
    size_t nnet = wcol.count;
    int any_named = 0;
    for (size_t i = 0; i < wcol.count; i++) {
        if (wcol.networks[i].ssid[0] != '\0') any_named = 1;
        CONF_CHECK(wcol.networks[i].frequency_mhz > 0,
                   "network %zu frequency 0", i);
        if (is_mock)
            CONF_CHECK(wcol.networks[i].signal_dbm < 0,
                       "mock network %zu signal not negative: %d", i,
                       (int)wcol.networks[i].signal_dbm);
        else
            CONF_CHECK(wcol.networks[i].signal_dbm == 0,
                       "linux network %zu signal not 0 (not "
                       "reported): %d",
                       i, (int)wcol.networks[i].signal_dbm);
    }
    rb_mutex_unlock(&wcol.mu);
    CONF_CHECK(nnet >= 1, "wifi scan found 0 networks");
    CONF_CHECK(any_named, "wifi scan found no named network");
    printf("CONFORMANCE %s: wifi scan found %zu network(s)\n",
           argv[1], nnet);

    /* Wi-Fi error mapping: unknown SSID -> NOT_FOUND sync. */
    st = rb_wifi_connect(ctx, "RB-No-Such-Network-XYZ", NULL, NULL);
    CONF_CHECK(st == RB_ERROR_NOT_FOUND,
               "wifi connect unknown -> %d", (int)st);

    rb_mutex_destroy(&scol.mu);
    rb_mutex_destroy(&wcol.mu);
    rb_shutdown(ctx);
    if (g_failures > 0) {
        printf("CONFORMANCE %s: FAIL (%d failures)\n", argv[1],
               g_failures);
        return 1;
    }
    printf("CONFORMANCE %s: PASS\n", argv[1]);
    return 0;
}
