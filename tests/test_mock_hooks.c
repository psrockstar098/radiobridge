/* Mock hooks: simulated drops + reconnect policy behavior. */
#include "test_common.h"

typedef struct {
    volatile int disconnected;
    volatile int reconnected;
    volatile int reconnect_note;
    volatile int failed;
} recon_collect_t;

static void on_conn(rb_context_t *ctx,
                    const rb_ble_connection_event_t *ev, void *ud) {
    (void)ctx;
    recon_collect_t *c = (recon_collect_t *)ud;
    if (ev->state == RB_BLE_DISCONNECTED) c->disconnected = 1;
    if (ev->state == RB_BLE_CONNECTED) c->reconnected = 1;
    if (ev->state == RB_BLE_FAILED) c->failed = 1;
    if (strstr(ev->message, "reconnect attempt") != NULL)
        c->reconnect_note = 1;
}

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    /* Fast policy so the test stays quick. */
    rb_reconnect_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.enabled = 1;
    policy.max_attempts = 3;
    policy.initial_delay_ms = 100;
    policy.backoff_factor_x100 = 200;
    CHECK_ST(rb_ble_set_reconnect_policy(ctx, &policy), RB_OK);

    rb_device_id_t hr;
    memset(&hr, 0, sizeof(hr));
    snprintf(hr.address, sizeof(hr.address), "AA:BB:CC:00:00:01");

    /* Trigger on a non-connected device is an error, not a crash. */
    CHECK_ST(rb_mock_trigger_disconnect(ctx, &hr),
             RB_ERROR_NOT_CONNECTED);

    /* Connect with auto-reconnect, drop, watch it come back. */
    recon_collect_t rc;
    memset(&rc, 0, sizeof(rc));
    rb_ble_connect_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.auto_reconnect = 1;
    rb_operation_id_t op = 0;
    CHECK_ST(rb_ble_connect(ctx, &hr, &opts, on_conn, &rc, &op), RB_OK);
    CHECK(test_wait_until(&rc.reconnected, 2000));
    rc.reconnected = 0;
    rc.disconnected = 0;
    CHECK_ST(rb_mock_trigger_disconnect(ctx, &hr), RB_OK);
    CHECK(test_wait_until(&rc.disconnected, 2000));
    CHECK(test_wait_until(&rc.reconnected, 3000));
    CHECK(rc.reconnect_note);
    CHECK(!rc.failed);

    /* Policy disabled: a drop stays dropped. */
    memset(&policy, 0, sizeof(policy));
    policy.enabled = 0;
    CHECK_ST(rb_ble_set_reconnect_policy(ctx, &policy), RB_OK);
    /* Reconnect (policy applies to future connections): disconnect
     * first, then connect without auto-reconnect. */
    CHECK_ST(rb_ble_disconnect(ctx, &hr), RB_OK);
    recon_collect_t rc2;
    memset(&rc2, 0, sizeof(rc2));
    memset(&opts, 0, sizeof(opts));
    opts.auto_reconnect = 0;
    CHECK_ST(rb_ble_connect(ctx, &hr, &opts, on_conn, &rc2, &op), RB_OK);
    CHECK(test_wait_until(&rc2.reconnected, 2000));
    rc2.reconnected = 0;
    CHECK_ST(rb_mock_trigger_disconnect(ctx, &hr), RB_OK);
    CHECK(test_wait_until(&rc2.disconnected, 2000));
    test_sleep_ms(700);
    CHECK(!rc2.reconnected);

    /* Mock hooks reject a NULL context cleanly. */
    CHECK_ST(rb_mock_trigger_disconnect(NULL, &hr),
             RB_ERROR_NOT_INITIALIZED);

    rb_shutdown(ctx);
    TEST_END();
}
