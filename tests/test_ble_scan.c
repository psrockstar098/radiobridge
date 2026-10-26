/* Mock BLE scan: full catalog, filters, timeout, stop, cancel. */
#include "core/rb_internal.h"
#include "test_common.h"

typedef struct {
    rb_ble_device_t found[16];
    volatile int count;
    rb_mutex_t lock;
} scan_collect_t;

static void on_scan(rb_context_t *ctx, const rb_ble_device_t *dev,
                    void *ud) {
    (void)ctx;
    scan_collect_t *c = (scan_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    if (c->count < 16) c->found[c->count] = *dev;
    c->count++;
    rb_mutex_unlock(&c->lock);
}

static int has_address(scan_collect_t *c, const char *addr) {
    int found = 0;
    rb_mutex_lock(&c->lock);
    for (int i = 0; i < c->count && i < 16; i++) {
        if (strcmp(c->found[i].address, addr) == 0) found = 1;
    }
    rb_mutex_unlock(&c->lock);
    return found;
}

/* Run one scan and return the number of devices found.
 * The caller owns the collector mutex: it is initialized once in main
 * and destroyed after the last use. This function only resets the
 * collected data, and it STOPS the scan before returning, so no
 * callback can fire into the collector after the call.
 *
 * The previous version destroyed the collector mutex inside this
 * function and then kept using it (has_address locks it), while the
 * scan thread could also still be running. POSIX silently tolerates
 * locking a destroyed mutex, so Linux/macOS passed; Windows
 * DeleteCriticalSection tears the object down, so the Windows run
 * segfaulted in test_ble_scan. The stop call below is what makes
 * the collector lifetime safe: it joins a still-running scan thread,
 * and when the thread already finished (NOT_FOUND) no callback can
 * fire afterwards either. */
static int run_scan(rb_context_t *ctx,
                    const rb_ble_scan_options_t *opts,
                    scan_collect_t *c, int expected) {
    c->count = 0;
    memset(c->found, 0, sizeof(c->found));
    rb_operation_id_t op = 0;
    rb_status_t st = rb_ble_scan_start(ctx, opts, on_scan, c, &op);
    if (st != RB_OK) {
        return -1;
    }
    if (expected > 0) {
        /* Wait for every expected delivery, however long the runner's
         * thread scheduling takes — never a fixed sleep. */
        test_wait_count(&c->count, expected, 5000);
    } else {
        /* Zero matches cannot be observed; delivery of the whole
         * catalog takes ~0.5s worst case, so wait generously. */
        test_sleep_ms(1200);
    }
    /* Stop (joins the thread when still running; NOT_FOUND when it
     * already finished). Either way no callback fires after this. */
    (void)rb_ble_scan_stop(ctx, op);
    return c->count;
}

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    scan_collect_t c;
    memset(&c, 0, sizeof(c));
    rb_mutex_init(&c.lock);
    rb_ble_scan_options_t opts;

    /* Full catalog, no filter. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    CHECK(run_scan(ctx, &opts, &c, 3) == 3);
    CHECK(has_address(&c, "AA:BB:CC:00:00:01"));
    CHECK(has_address(&c, "AA:BB:CC:00:00:02"));
    CHECK(has_address(&c, "AA:BB:CC:00:00:03"));

    /* Service-UUID filter: Heart Rate service only. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    CHECK_ST(rb_uuid_from_16(0x180D, &opts.service_uuids[0]), RB_OK);
    opts.service_uuid_count = 1;
    CHECK(run_scan(ctx, &opts, &c, 1) == 1);
    CHECK(has_address(&c, "AA:BB:CC:00:00:01"));

    /* Manufacturer filter. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = 0x004C;
    CHECK(run_scan(ctx, &opts, &c, 1) == 1);
    CHECK(has_address(&c, "AA:BB:CC:00:00:02"));

    /* RSSI floor: only the -52 dBm device passes -60. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    opts.has_min_rssi = 1;
    opts.min_rssi_dbm = -60;
    CHECK(run_scan(ctx, &opts, &c, 1) == 1);
    CHECK(has_address(&c, "AA:BB:CC:00:00:01"));

    /* Name substring, case-insensitive. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    rb_copy_str(opts.name_substring, sizeof(opts.name_substring),
                "DEMO");
    CHECK(run_scan(ctx, &opts, &c, 1) == 1);
    CHECK(has_address(&c, "AA:BB:CC:00:00:03"));

    /* Combined filter: thermometer service + RSSI floor -70. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    CHECK_ST(rb_uuid_from_16(0x1809, &opts.service_uuids[0]), RB_OK);
    opts.service_uuid_count = 1;
    opts.has_min_rssi = 1;
    opts.min_rssi_dbm = -70;
    CHECK(run_scan(ctx, &opts, &c, 1) == 1);
    CHECK(has_address(&c, "AA:BB:CC:00:00:02"));

    /* Unknown service matches nothing. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    CHECK_ST(rb_uuid_from_16(0x181A, &opts.service_uuids[0]), RB_OK);
    opts.service_uuid_count = 1;
    CHECK(run_scan(ctx, &opts, &c, 0) == 0);

    /* Timeout: 50 ms lets the first device through, then stops. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    opts.timeout_ms = 50;
    CHECK(run_scan(ctx, &opts, &c, 1) == 1);

    /* Second concurrent scan is rejected. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    scan_collect_t c1;
    memset(&c1, 0, sizeof(c1));
    rb_mutex_init(&c1.lock);
    rb_operation_id_t op1 = 0, op2 = 0;
    CHECK_ST(rb_ble_scan_start(ctx, &opts, on_scan, &c1, &op1), RB_OK);
    CHECK_ST(rb_ble_scan_start(ctx, &opts, on_scan, &c1, &op2),
             RB_ERROR_ALREADY_IN_PROGRESS);
    CHECK_ST(rb_ble_scan_stop(ctx, 999999), RB_ERROR_NOT_FOUND);
    CHECK_ST(rb_ble_scan_stop(ctx, op1), RB_OK);
    test_sleep_ms(300);
    rb_mutex_destroy(&c1.lock);

    /* A fresh scan works after the stopped one. */
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    CHECK(run_scan(ctx, &opts, &c, 3) == 3);

    rb_mutex_destroy(&c.lock);
    rb_shutdown(ctx);
    TEST_END();
}
