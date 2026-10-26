/* Mock Wi-Fi: status, scan, connect, quality, events, diagnostics. */
#include "test_common.h"

typedef struct {
    rb_wifi_network_t found[16];
    volatile int count;
    rb_mutex_t lock;
} wifi_collect_t;

static void on_wifi_scan(rb_context_t *ctx,
                         const rb_wifi_network_t *net, void *ud) {
    (void)ctx;
    wifi_collect_t *c = (wifi_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    if (c->count < 16) c->found[c->count] = *net;
    c->count++;
    rb_mutex_unlock(&c->lock);
}

typedef struct {
    volatile int changed;
    volatile int count;
} event_collect_t;

static void on_event(rb_context_t *ctx, const rb_event_t *ev, void *ud) {
    (void)ctx;
    (void)ev;
    event_collect_t *c = (event_collect_t *)ud;
    c->changed = 1;
    c->count++;
}

static int has_ssid(wifi_collect_t *c, const char *ssid) {
    int found = 0;
    rb_mutex_lock(&c->lock);
    for (int i = 0; i < c->count && i < 16; i++) {
        if (strcmp(c->found[i].ssid, ssid) == 0) found = 1;
    }
    rb_mutex_unlock(&c->lock);
    return found;
}

/* Poll the mock state until it reaches `want` (or the timeout hits).
 * Async completion must be awaited, not slept for: on a loaded CI
 * runner thread scheduling can push a fixed sleep past the point
 * where the scan/connect thread has delivered, which is exactly how
 * the macOS run failed wc.count == 3 after a flat 800 ms sleep. */
static int wait_wifi_state(rb_context_t *ctx, rb_wifi_state_t want,
                           int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        rb_wifi_status_t st;
        if (rb_wifi_get_status(ctx, &st) == RB_OK &&
            st.state == want)
            return 1;
        test_sleep_ms(10);
        waited += 10;
    }
    return 0;
}

int main(void) {
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    event_collect_t ec;
    memset(&ec, 0, sizeof(ec));
    cfg.event_callback = on_event;
    cfg.event_user_data = &ec;
    rb_context_t *ctx = NULL;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    /* Initial state: disconnected, diagnostic matches. */
    rb_wifi_status_t st;
    CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
    CHECK(st.state == RB_WIFI_DISCONNECTED);
    CHECK(st.diag == RB_WIFI_DIAG_DISCONNECTED);

    /* Quality while disconnected is an error, not a fabrication. */
    rb_wifi_quality_t q;
    CHECK_ST(rb_wifi_measure_quality(ctx, NULL, &q),
             RB_ERROR_NOT_CONNECTED);
    CHECK(q.reachable == 0);

    /* Scan finds the catalog. */
    wifi_collect_t wc;
    memset(&wc, 0, sizeof(wc));
    rb_mutex_init(&wc.lock);
    rb_wifi_scan_options_t sopts;
    memset(&sopts, 0, sizeof(sopts));
    rb_operation_id_t sop = 0;
    CHECK_ST(rb_wifi_scan_start(ctx, &sopts, on_wifi_scan, &wc, &sop),
             RB_OK);
    /* Second concurrent scan is rejected. */
    rb_operation_id_t sop2 = 0;
    CHECK_ST(rb_wifi_scan_start(ctx, &sopts, on_wifi_scan, &wc, &sop2),
             RB_ERROR_ALREADY_IN_PROGRESS);
    CHECK_ST(rb_wifi_scan_stop(ctx, 999999), RB_ERROR_NOT_FOUND);
    /* Wait for all three scan deliveries, however long the runner's
     * thread scheduling takes — never a fixed sleep. */
    CHECK(test_wait_count(&wc.count, 3, 5000));
    CHECK(wc.count == 3);
    CHECK(has_ssid(&wc, "RB-HomeNet"));
    CHECK(has_ssid(&wc, "RB-CoffeeShop"));
    CHECK(has_ssid(&wc, "RB-Lab-5G"));
    rb_mutex_destroy(&wc.lock);

    /* Unknown SSID, open-network auth, secured-network auth. */
    CHECK_ST(rb_wifi_connect(ctx, "No-Such-Net", NULL, NULL),
             RB_ERROR_NOT_FOUND);
    CHECK_ST(rb_wifi_connect(ctx, "RB-HomeNet", NULL, NULL),
             RB_ERROR_AUTH_FAILED);

    /* Open network connects: status + metadata + event. */
    ec.changed = 0;
    CHECK_ST(rb_wifi_connect(ctx, "RB-CoffeeShop", NULL, NULL), RB_OK);
    CHECK(wait_wifi_state(ctx, RB_WIFI_CONNECTED, 5000));
    CHECK(test_wait_until(&ec.changed, 2000));
    CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
    CHECK(st.state == RB_WIFI_CONNECTED);
    CHECK(strcmp(st.ssid, "RB-CoffeeShop") == 0);
    CHECK(strcmp(st.ip_address, "192.168.7.42") == 0);
    CHECK(st.signal_dbm == -62);

    /* Quality while connected: reachable, sane simulated numbers. */
    CHECK_ST(rb_wifi_measure_quality(ctx, NULL, &q), RB_OK);
    CHECK(q.reachable == 1);
    CHECK(q.latency_ms > 0.0);
    CHECK(q.packet_loss_percent == 0.0);
    CHECK(strcmp(q.endpoint, "rb.test.local") == 0);
    CHECK_ST(rb_wifi_measure_quality(ctx, "example.test", &q), RB_OK);
    CHECK(strcmp(q.endpoint, "example.test") == 0);

    /* Disconnect: state + diagnostic return to disconnected. */
    CHECK_ST(rb_wifi_disconnect(ctx), RB_OK);
    CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
    CHECK(st.state == RB_WIFI_DISCONNECTED);
    CHECK(st.diag == RB_WIFI_DIAG_DISCONNECTED);

    /* Secured network with a password connects. */
    CHECK_ST(rb_wifi_connect(ctx, "RB-HomeNet", "correct-horse", NULL),
             RB_OK);
    CHECK(wait_wifi_state(ctx, RB_WIFI_CONNECTED, 5000));
    CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
    CHECK(st.state == RB_WIFI_CONNECTED);
    CHECK(strcmp(st.ssid, "RB-HomeNet") == 0);

    /* Mock trigger drives a change event without real interfaces. */
    ec.changed = 0;
    CHECK_ST(rb_mock_trigger_wifi_change(ctx, RB_WIFI_DISCONNECTED),
             RB_OK);
    CHECK(test_wait_until(&ec.changed, 2000));

    rb_shutdown(ctx);
    TEST_END();
}
