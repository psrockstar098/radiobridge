/* Cancellation: scan stop, rb_cancel, connect cancel. */
#include "test_common.h"

typedef struct {
    volatile int count;
} count_collect_t;

static void on_scan(rb_context_t *ctx, const rb_ble_device_t *dev,
                    void *ud) {
    (void)ctx;
    (void)dev;
    count_collect_t *c = (count_collect_t *)ud;
    c->count++;
}

typedef struct {
    volatile int failed_cancelled;
    volatile int connected;
} conn_flag_t;

static void on_conn(rb_context_t *ctx,
                    const rb_ble_connection_event_t *ev, void *ud) {
    (void)ctx;
    conn_flag_t *f = (conn_flag_t *)ud;
    if (ev->state == RB_BLE_FAILED && ev->status == RB_ERROR_CANCELLED)
        f->failed_cancelled = 1;
    if (ev->state == RB_BLE_CONNECTED) f->connected = 1;
}

static void on_wifi_scan(rb_context_t *ctx,
                         const rb_wifi_network_t *net, void *ud) {
    (void)ctx;
    (void)net;
    count_collect_t *c = (count_collect_t *)ud;
    c->count++;
}

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    /* Argument validation. */
    CHECK_ST(rb_cancel(ctx, 0), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_cancel(ctx, 999999), RB_ERROR_NOT_FOUND);
    CHECK_ST(rb_cancel(NULL, 1), RB_ERROR_INVALID_ARGUMENT);

    /* BLE scan cancelled mid-flight stops deliveries. */
    count_collect_t cc;
    memset(&cc, 0, sizeof(cc));
    rb_ble_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    rb_operation_id_t op = 0;
    CHECK_ST(rb_ble_scan_start(ctx, &opts, on_scan, &cc, &op), RB_OK);
    CHECK_ST(rb_cancel(ctx, op), RB_OK);
    int count_after_cancel = cc.count;
    test_sleep_ms(600);
    /* rb_cancel joins the scan thread, so the count is frozen: no
     * further deliveries may appear after cancel returns. */
    CHECK(cc.count <= 3);
    CHECK(cc.count == count_after_cancel);

    /* A fresh scan works after cancellation. */
    memset(&cc, 0, sizeof(cc));
    CHECK_ST(rb_ble_scan_start(ctx, &opts, on_scan, &cc, &op), RB_OK);
    test_sleep_ms(1200);
    CHECK(cc.count == 3);

    /* Connect cancelled before it completes: CANCELLED, not CONNECTED. */
    conn_flag_t cf;
    memset(&cf, 0, sizeof(cf));
    rb_device_id_t dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.address, sizeof(dev.address), "AA:BB:CC:00:00:01");
    rb_operation_id_t cop = 0;
    CHECK_ST(rb_ble_connect(ctx, &dev, NULL, on_conn, &cf, &cop), RB_OK);
    CHECK_ST(rb_cancel(ctx, cop), RB_OK);
    CHECK(test_wait_until(&cf.failed_cancelled, 2000));
    CHECK(!cf.connected);

    /* Wi-Fi scan cancel. */
    count_collect_t wc;
    memset(&wc, 0, sizeof(wc));
    rb_wifi_scan_options_t wopts;
    memset(&wopts, 0, sizeof(wopts));
    rb_operation_id_t wop = 0;
    CHECK_ST(rb_wifi_scan_start(ctx, &wopts, on_wifi_scan, &wc, &wop),
             RB_OK);
    CHECK_ST(rb_cancel(ctx, wop), RB_OK);
    test_sleep_ms(400);

    rb_shutdown(ctx);
    TEST_END();
}
