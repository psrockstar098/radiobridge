/* Linux backend over a fake bus: the backend under test talks to a
 * fake BlueZ + NetworkManager service (tests/fake_bus/fake_bus.c)
 * over a real private D-Bus daemon started by this test. No hardware,
 * no system bus, no mocks inside the library — the transport,
 * interface calls, and reply parsing are all exercised for real.
 *
 * The test starts:
 *   dbus-daemon --session --nofork --print-address  (address on stdout)
 *   rb_fake_bus (RB_DBUS_ADDRESS set to that address)
 * then runs the full operation sequence against RB_BACKEND_LINUX and
 * kills both children at the end.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/rb_internal.h"
#include "platform/linux/rb_dbus.h"
#include "radiobridge/rb_bluetooth.h"
#include "radiobridge/rb_diagnostics.h"
#include "radiobridge/rb_wifi.h"
#include "test_common.h"

#ifndef RB_FAKE_BUS_BIN
#error "RB_FAKE_BUS_BIN must be defined by CMake"
#endif

/* --- Collectors (mutex-protected; callbacks fire from worker threads) --- */

typedef struct {
    rb_mutex_t lock;
    rb_ble_device_t devices[8];
    size_t count;
} scan_collect_t;

static void scan_cb(rb_context_t *ctx, const rb_ble_device_t *dev,
                    void *ud) {
    (void)ctx;
    scan_collect_t *c = (scan_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    if (c->count < 8) c->devices[c->count++] = *dev;
    rb_mutex_unlock(&c->lock);
}

static size_t scan_count(scan_collect_t *c) {
    rb_mutex_lock(&c->lock);
    size_t n = c->count;
    rb_mutex_unlock(&c->lock);
    return n;
}

typedef struct {
    rb_mutex_t lock;
    rb_ble_connection_state_t state;
    rb_status_t status;
} conn_collect_t;

static void conn_cb(rb_context_t *ctx, const rb_ble_connection_event_t *ev,
                    void *ud) {
    (void)ctx;
    conn_collect_t *c = (conn_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    c->state = ev->state;
    c->status = ev->status;
    rb_mutex_unlock(&c->lock);
}

typedef struct {
    rb_mutex_t lock;
    size_t count;
    uint8_t data[RB_BLE_MAX_VALUE_LEN];
    size_t len;
} notify_collect_t;

static void notify_cb(rb_context_t *ctx, const rb_ble_notification_t *n,
                      void *ud) {
    (void)ctx;
    notify_collect_t *c = (notify_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    c->count++;
    c->len = n->data_len;
    if (n->data_len > 0) memcpy(c->data, n->data, n->data_len);
    rb_mutex_unlock(&c->lock);
}

typedef struct {
    rb_mutex_t lock;
    rb_wifi_network_t nets[8];
    size_t count;
} wifi_collect_t;

static void wifi_cb(rb_context_t *ctx, const rb_wifi_network_t *net,
                    void *ud) {
    (void)ctx;
    wifi_collect_t *c = (wifi_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    if (c->count < 8) c->nets[c->count++] = *net;
    rb_mutex_unlock(&c->lock);
}

static int wait_until(int (*cond)(void *), void *ud, unsigned int ms) {
    unsigned int waited = 0;
    while (waited < ms) {
        if (cond(ud)) return 1;
        rb_sleep_ms(50);
        waited += 50;
    }
    return cond(ud);
}

static int cond_scan2(void *ud) { return scan_count((scan_collect_t *)ud) >= 2; }
static int cond_scan1(void *ud) { return scan_count((scan_collect_t *)ud) >= 1; }
static int cond_connected(void *ud) {
    conn_collect_t *c = (conn_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    int ok = c->state == RB_BLE_CONNECTED;
    rb_mutex_unlock(&c->lock);
    return ok;
}
static int cond_notified(void *ud) {
    notify_collect_t *c = (notify_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    int ok = c->count > 0;
    rb_mutex_unlock(&c->lock);
    return ok;
}
static int cond_wifi2(void *ud) {
    wifi_collect_t *c = (wifi_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    int ok = c->count >= 2;
    rb_mutex_unlock(&c->lock);
    return ok;
}

typedef struct {
    rb_context_t *ctx;
    rb_wifi_state_t want;
} status_want_t;

static int cond_status(void *ud) {
    status_want_t *w = (status_want_t *)ud;
    rb_wifi_status_t st;
    if (rb_wifi_get_status(w->ctx, &st) != RB_OK) return 0;
    return st.state == w->want;
}

static int names_ready(void *ud) {
    (void)ud;
    char detail[256] = "";
    rb_dbus_conn_t *bus = NULL;
    if (rb_dbus_open(&bus, detail, sizeof(detail)) != RB_OK) return 0;
    int b = rb_dbus_name_has_owner(bus, "org.bluez", detail,
                                   sizeof(detail));
    int n = rb_dbus_name_has_owner(bus, "org.freedesktop.NetworkManager",
                                   detail, sizeof(detail));
    rb_dbus_close(bus);
    return b == 1 && n == 1;
}

int main(void) {
    pid_t daemon_pid = -1, fake_pid = -1;
    rb_context_t *ctx = NULL;

    /* 1. Private daemon; read its address from stdout. */
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    daemon_pid = fork();
    CHECK(daemon_pid >= 0);
    if (daemon_pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork",
               "--print-address", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    FILE *addr_f = fdopen(pipefd[0], "r");
    CHECK(addr_f != NULL);
    char bus_addr[512] = "";
    if (addr_f && fgets(bus_addr, sizeof(bus_addr), addr_f) == NULL) {
        printf("FAIL: dbus-daemon printed no address\n");
        g_rb_test_failures++;
    }
    /* Strip trailing newline. */
    size_t alen = strlen(bus_addr);
    while (alen > 0 &&
           (bus_addr[alen - 1] == '\n' || bus_addr[alen - 1] == '\r'))
        bus_addr[--alen] = '\0';
    CHECK(alen > 0);
    setenv("RB_DBUS_ADDRESS", bus_addr, 1);

    /* 2. Fake service on the same bus. */
    fake_pid = fork();
    CHECK(fake_pid >= 0);
    if (fake_pid == 0) {
        execlp(RB_FAKE_BUS_BIN, RB_FAKE_BUS_BIN, (char *)NULL);
        _exit(127);
    }
    CHECK(wait_until(names_ready, NULL, 10000));

    /* 3. Init + caps + diagnostics. */
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_LINUX;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);
    if (ctx == NULL) {
        printf("FAIL: init failed, cannot continue bus test\n");
        g_rb_test_failures++;
        goto cleanup;
    }
    rb_capabilities_t caps;
    CHECK_ST(rb_get_capabilities(ctx, &caps), RB_OK);
    CHECK(caps.ble.scan == RB_CAP_AVAILABLE);
    CHECK(caps.ble.connect == RB_CAP_AVAILABLE);
    CHECK(caps.ble.gatt == RB_CAP_AVAILABLE);
    CHECK(caps.ble.notify == RB_CAP_AVAILABLE);
    CHECK(caps.ble.pairing == RB_CAP_AVAILABLE);
    CHECK(caps.ble.rssi == RB_CAP_AVAILABLE);
    CHECK(caps.ble.reconnect == RB_CAP_UNAVAILABLE);
    CHECK(caps.wifi.status == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.scan == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.connect == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.quality == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.saved_networks == RB_CAP_NOT_SUPPORTED);

    rb_diagnostics_t diag;
    CHECK_ST(rb_get_diagnostics(ctx, &diag), RB_OK);
    CHECK(diag.bluez_present == 1);
    CHECK(diag.nm_present == 1);
    CHECK(strcmp(diag.nm_version, "1.40.0-rb-fake") == 0);
    CHECK(strcmp(diag.adapter_path, "/org/bluez/hci0") == 0);
    CHECK(strcmp(diag.adapter_address, "00:11:22:33:44:55") == 0);
    CHECK(diag.adapter_powered == 1);
    CHECK(diag.wifi_device_path[0] != '\0');
    CHECK(strstr(diag.bus_address, bus_addr) != NULL ||
          strstr(bus_addr, diag.bus_address) != NULL ||
          diag.bus_address[0] != '\0');

    /* 4. BLE scan: all devices. */
    {
        scan_collect_t sc;
        memset(&sc, 0, sizeof(sc));
        rb_mutex_init(&sc.lock);
        rb_ble_scan_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.manufacturer_id = -1;
        rb_operation_id_t op = 0;
        CHECK_ST(rb_ble_scan_start(ctx, &opts, scan_cb, &sc, &op),
                 RB_OK);
        CHECK(wait_until(cond_scan2, &sc, 5000));
        CHECK_ST(rb_ble_scan_stop(ctx, op), RB_OK);
        rb_mutex_lock(&sc.lock);
        CHECK(sc.count == 2);
        int found_heart = 0, found_thermo = 0;
        for (size_t i = 0; i < sc.count; i++) {
            if (strcmp(sc.devices[i].address, "AA:BB:CC:00:00:01") == 0) {
                found_heart = 1;
                CHECK(sc.devices[i].rssi_dbm == -52);
                CHECK(strcmp(sc.devices[i].name, "RB Fake Heart") == 0);
                CHECK(sc.devices[i].has_manufacturer_data == 1);
                CHECK(sc.devices[i].manufacturer_id == 0x004c);
            }
            if (strcmp(sc.devices[i].address, "AA:BB:CC:00:00:02") == 0) {
                found_thermo = 1;
                CHECK(sc.devices[i].rssi_dbm == -68);
            }
        }
        CHECK(found_heart == 1);
        CHECK(found_thermo == 1);
        rb_mutex_unlock(&sc.lock);
        rb_mutex_destroy(&sc.lock);
    }

    /* 5. BLE scan: service-UUID filter (1809 -> thermo only). */
    {
        scan_collect_t sc;
        memset(&sc, 0, sizeof(sc));
        rb_mutex_init(&sc.lock);
        rb_ble_scan_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.manufacturer_id = -1;
        CHECK_ST(rb_uuid_from_16(0x1809, &opts.service_uuids[0]),
                 RB_OK);
        opts.service_uuid_count = 1;
        rb_operation_id_t op = 0;
        CHECK_ST(rb_ble_scan_start(ctx, &opts, scan_cb, &sc, &op),
                 RB_OK);
        CHECK(wait_until(cond_scan1, &sc, 5000));
        CHECK_ST(rb_ble_scan_stop(ctx, op), RB_OK);
        rb_mutex_lock(&sc.lock);
        CHECK(sc.count == 1);
        if (sc.count == 1)
            CHECK(strcmp(sc.devices[0].address, "AA:BB:CC:00:00:02") ==
                  0);
        rb_mutex_unlock(&sc.lock);
        rb_mutex_destroy(&sc.lock);
    }

    /* 6. BLE scan: RSSI floor -60 -> heart only. */
    {
        scan_collect_t sc;
        memset(&sc, 0, sizeof(sc));
        rb_mutex_init(&sc.lock);
        rb_ble_scan_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.manufacturer_id = -1;
        opts.has_min_rssi = 1;
        opts.min_rssi_dbm = -60;
        rb_operation_id_t op = 0;
        CHECK_ST(rb_ble_scan_start(ctx, &opts, scan_cb, &sc, &op),
                 RB_OK);
        CHECK(wait_until(cond_scan1, &sc, 5000));
        CHECK_ST(rb_ble_scan_stop(ctx, op), RB_OK);
        rb_mutex_lock(&sc.lock);
        CHECK(sc.count == 1);
        if (sc.count == 1)
            CHECK(strcmp(sc.devices[0].address, "AA:BB:CC:00:00:01") ==
                  0);
        rb_mutex_unlock(&sc.lock);
        rb_mutex_destroy(&sc.lock);
    }

    /* 7. Connect / discover / read / write / notify / RSSI /
     * disconnect on the heart device. */
    rb_uuid_t svc_heart, chr_heart, svc_thermo;
    CHECK_ST(rb_uuid_from_16(0x180d, &svc_heart), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x2a37, &chr_heart), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x1809, &svc_thermo), RB_OK);
    rb_device_id_t dev1;
    memset(&dev1, 0, sizeof(dev1));
    rb_copy_str(dev1.address, sizeof(dev1.address), "AA:BB:CC:00:00:01");
    {
        conn_collect_t cc;
        memset(&cc, 0, sizeof(cc));
        rb_mutex_init(&cc.lock);
        rb_operation_id_t op = 0;
        CHECK_ST(rb_ble_connect(ctx, &dev1, NULL, conn_cb, &cc, &op),
                 RB_OK);
        CHECK(wait_until(cond_connected, &cc, 10000));
        rb_mutex_destroy(&cc.lock);
    }
    rb_ble_service_t *services = NULL;
    size_t service_count = 0;
    CHECK_ST(rb_ble_discover(ctx, &dev1, &services, &service_count),
             RB_OK);
    CHECK(service_count == 1);
    if (service_count == 1) {
        CHECK(rb_uuid_equals(&services[0].uuid, &svc_heart));
        CHECK(services[0].characteristic_count == 1);
        if (services[0].characteristic_count == 1) {
            CHECK(rb_uuid_equals(&services[0].characteristics[0].uuid,
                                 &chr_heart));
            CHECK((services[0].characteristics[0].properties &
                   RB_GATT_PROP_READ) != 0);
            CHECK((services[0].characteristics[0].properties &
                   RB_GATT_PROP_NOTIFY) != 0);
            CHECK(services[0].characteristics[0].descriptor_count ==
                  1);
        }
    }
    uint8_t value[RB_BLE_MAX_VALUE_LEN];
    size_t value_len = 0;
    CHECK_ST(rb_ble_read(ctx, &dev1, &svc_heart, &chr_heart, value,
                         &value_len),
             RB_OK);
    CHECK(value_len == 2);
    CHECK(value[0] == 0x48 && value[1] == 0x00);
    uint8_t wr[2] = {0xaa, 0xbb};
    CHECK_ST(rb_ble_write(ctx, &dev1, &svc_heart, &chr_heart, wr, 2, 1),
             RB_OK);
    {
        notify_collect_t nc;
        memset(&nc, 0, sizeof(nc));
        rb_mutex_init(&nc.lock);
        CHECK_ST(rb_ble_subscribe(ctx, &dev1, &svc_heart, &chr_heart,
                                  notify_cb, &nc),
                 RB_OK);
        CHECK(wait_until(cond_notified, &nc, 5000));
        rb_mutex_lock(&nc.lock);
        CHECK(nc.count >= 1);
        CHECK(nc.len == 2);
        CHECK(nc.data[0] == 0x49 && nc.data[1] == 0x00);
        rb_mutex_unlock(&nc.lock);
        CHECK_ST(rb_ble_unsubscribe(ctx, &dev1, &svc_heart, &chr_heart),
                 RB_OK);
        rb_mutex_destroy(&nc.lock);
    }
    int8_t rssi = 0;
    CHECK_ST(rb_ble_get_rssi(ctx, &dev1, &rssi), RB_OK);
    CHECK(rssi == -52);
    CHECK_ST(rb_ble_disconnect(ctx, &dev1), RB_OK);
    if (services) rb_free(services);

    /* 8. Wi-Fi status + scan. */
    {
        rb_wifi_status_t st;
        CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
        CHECK(st.state == RB_WIFI_DISCONNECTED);
    }
    {
        wifi_collect_t wc;
        memset(&wc, 0, sizeof(wc));
        rb_mutex_init(&wc.lock);
        rb_wifi_scan_options_t opts;
        memset(&opts, 0, sizeof(opts));
        rb_operation_id_t op = 0;
        CHECK_ST(rb_wifi_scan_start(ctx, &opts, wifi_cb, &wc, &op),
                 RB_OK);
        CHECK(wait_until(cond_wifi2, &wc, 5000));
        rb_mutex_lock(&wc.lock);
        CHECK(wc.count == 2);
        int found_open = 0, found_sec = 0;
        for (size_t i = 0; i < wc.count; i++) {
            if (strcmp(wc.nets[i].ssid, "RB-FakeNet") == 0) {
                found_open = 1;
                CHECK(wc.nets[i].security == RB_WIFI_SEC_OPEN);
                CHECK(wc.nets[i].frequency_mhz == 2412);
                CHECK(wc.nets[i].channel == 1);
                CHECK(wc.nets[i].signal_dbm == 0); /* percent, not dBm */
            }
            if (strcmp(wc.nets[i].ssid, "RB-FakeSecure") == 0) {
                found_sec = 1;
                CHECK(wc.nets[i].security == RB_WIFI_SEC_WPA2);
                CHECK(wc.nets[i].frequency_mhz == 5180);
                CHECK(wc.nets[i].channel == 36);
            }
        }
        CHECK(found_open == 1);
        CHECK(found_sec == 1);
        rb_mutex_unlock(&wc.lock);
        /* The wifi scan thread ends after one snapshot pass; stop is
         * best-effort here (it may already be done). */
        rb_wifi_scan_stop(ctx, op);
        rb_mutex_destroy(&wc.lock);
    }

    /* 9. Wi-Fi connect open -> status -> disconnect. */
    CHECK_ST(rb_wifi_connect(ctx, "RB-FakeNet", NULL, NULL), RB_OK);
    {
        status_want_t w = {ctx, RB_WIFI_CONNECTED};
        CHECK(wait_until(cond_status, &w, 5000));
        rb_wifi_status_t st;
        CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
        CHECK(st.state == RB_WIFI_CONNECTED);
        CHECK(strcmp(st.ssid, "RB-FakeNet") == 0);
        CHECK(strcmp(st.ip_address, "192.168.7.42") == 0);
        CHECK(strcmp(st.gateway, "192.168.7.1") == 0);
    }
    CHECK_ST(rb_wifi_disconnect(ctx), RB_OK);
    {
        status_want_t w = {ctx, RB_WIFI_DISCONNECTED};
        CHECK(wait_until(cond_status, &w, 5000));
    }
    /* Disconnect when already disconnected is OK per contract. */
    CHECK_ST(rb_wifi_disconnect(ctx), RB_OK);

    /* 10. Wi-Fi connect secured: no password -> AUTH_FAILED; with
     * password -> connected. */
    CHECK_ST(rb_wifi_connect(ctx, "RB-FakeSecure", NULL, NULL),
             RB_ERROR_AUTH_FAILED);
    CHECK_ST(rb_wifi_connect(ctx, "RB-FakeSecure", "test-password-123",
                             NULL),
             RB_OK);
    {
        status_want_t w = {ctx, RB_WIFI_CONNECTED};
        CHECK(wait_until(cond_status, &w, 5000));
        rb_wifi_status_t st;
        CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
        CHECK(strcmp(st.ssid, "RB-FakeSecure") == 0);
    }
    CHECK_ST(rb_wifi_disconnect(ctx), RB_OK);

    /* 11. Quality: real TCP probe against a local listener, and a
     * refused port. */
    {
        int listener = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listener >= 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ==
              0);
        CHECK(listen(listener, 4) == 0);
        struct sockaddr_in bound;
        socklen_t blen = sizeof(bound);
        CHECK(getsockname(listener, (struct sockaddr *)&bound, &blen) ==
              0);
        char endpoint[64];
        snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u",
                 (unsigned int)ntohs(bound.sin_port));
        rb_wifi_quality_t q;
        CHECK_ST(rb_wifi_measure_quality(ctx, endpoint, &q), RB_OK);
        CHECK(q.reachable == 1);
        CHECK(q.packet_loss_percent == 0.0);
        CHECK(q.latency_ms >= 0.0);
        close(listener);
        CHECK_ST(rb_wifi_measure_quality(ctx, "127.0.0.1:1", &q),
                 RB_OK);
        CHECK(q.reachable == 0);
        CHECK(q.packet_loss_percent == 100.0);
    }

cleanup:
    if (ctx) rb_shutdown(ctx);
    if (addr_f) fclose(addr_f);
    if (fake_pid > 0) {
        kill(fake_pid, SIGTERM);
        int status = 0;
        waitpid(fake_pid, &status, 0);
    }
    if (daemon_pid > 0) {
        kill(daemon_pid, SIGTERM);
        int status = 0;
        waitpid(daemon_pid, &status, 0);
    }
    unsetenv("RB_DBUS_ADDRESS");

    TEST_END();
}
