/* Callback threading: callbacks arrive off the caller thread, and
 * concurrent contexts/operations stay correct. */
#include "test_common.h"

typedef struct {
    volatile int count;
    volatile int saw_other_thread;
    pthread_t main_thread;
} thread_collect_t;

static void on_scan(rb_context_t *ctx, const rb_ble_device_t *dev,
                    void *ud) {
    (void)ctx;
    (void)dev;
    thread_collect_t *c = (thread_collect_t *)ud;
    if (!pthread_equal(pthread_self(), c->main_thread))
        c->saw_other_thread = 1;
    c->count++;
}

typedef struct {
    rb_context_t *ctx;
    thread_collect_t *collect;
    int result_count;
} worker_arg_t;

static void *scan_worker(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    rb_ble_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    rb_operation_id_t op = 0;
    rb_status_t st = rb_ble_scan_start(w->ctx, &opts, on_scan,
                                        w->collect, &op);
    if (st != RB_OK) {
        w->result_count = -1;
        return NULL;
    }
    test_sleep_ms(1200);
    w->result_count = w->collect->count;
    return NULL;
}

static void *caps_worker(void *arg) {
    rb_context_t *ctx = (rb_context_t *)arg;
    for (int i = 0; i < 50; i++) {
        rb_capabilities_t caps;
        if (rb_get_capabilities(ctx, &caps) != RB_OK) return (void *)1;
        if (strcmp(caps.backend_name, "mock") != 0) return (void *)1;
    }
    return NULL;
}

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    /* Callbacks arrive on a non-caller thread. */
    thread_collect_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.main_thread = pthread_self();
    rb_ble_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    rb_operation_id_t op = 0;
    CHECK_ST(rb_ble_scan_start(ctx, &opts, on_scan, &tc, &op), RB_OK);
    CHECK(test_wait_count(&tc.count, 3, 3000));
    CHECK(tc.saw_other_thread);
    test_sleep_ms(300);

    /* Four contexts scanning concurrently, each complete. */
    rb_context_t *ctxs[4];
    thread_collect_t cols[4];
    worker_arg_t args[4];
    pthread_t ths[4];
    for (int i = 0; i < 4; i++) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.backend = RB_BACKEND_MOCK;
        cfg.log_level = RB_LOG_WARN;
        CHECK_ST(rb_initialize(&cfg, &ctxs[i]), RB_OK);
        memset(&cols[i], 0, sizeof(cols[i]));
        cols[i].main_thread = pthread_self();
        args[i].ctx = ctxs[i];
        args[i].collect = &cols[i];
        args[i].result_count = 0;
        CHECK(pthread_create(&ths[i], NULL, scan_worker, &args[i]) == 0);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(ths[i], NULL);
        CHECK(args[i].result_count == 3);
        rb_shutdown(ctxs[i]);
    }

    /* Concurrent capability reads never fail or garble. */
    pthread_t cths[4];
    for (int i = 0; i < 4; i++)
        CHECK(pthread_create(&cths[i], NULL, caps_worker, ctx) == 0);
    for (int i = 0; i < 4; i++) {
        void *ret = NULL;
        pthread_join(cths[i], &ret);
        CHECK(ret == NULL);
    }

    rb_shutdown(ctx);
    TEST_END();
}
