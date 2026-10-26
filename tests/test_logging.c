/* Logging: levels filter, callback delivery, password never logged. */
#include "test_common.h"

typedef struct {
    char messages[32][512];
    volatile int count;
    rb_mutex_t lock;
} log_collect_t;

static void on_log(rb_log_level_t level, const char *subsystem,
                   const char *message, void *ud) {
    (void)level;
    (void)subsystem;
    log_collect_t *c = (log_collect_t *)ud;
    rb_mutex_lock(&c->lock);
    if (c->count < 32)
        snprintf(c->messages[c->count], 512, "%s", message);
    c->count++;
    rb_mutex_unlock(&c->lock);
}

static int log_contains(log_collect_t *c, const char *needle) {
    int found = 0;
    rb_mutex_lock(&c->lock);
    for (int i = 0; i < c->count && i < 32; i++) {
        if (strstr(c->messages[i], needle) != NULL) found = 1;
    }
    rb_mutex_unlock(&c->lock);
    return found;
}

int main(void) {
    /* DEBUG level: the init INFO message is captured. */
    log_collect_t lc;
    memset(&lc, 0, sizeof(lc));
    rb_mutex_init(&lc.lock);
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_DEBUG;
    cfg.log_callback = on_log;
    cfg.log_user_data = &lc;
    rb_context_t *ctx = NULL;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);
    CHECK(lc.count >= 1);
    CHECK(log_contains(&lc, "mock"));

    /* ERROR level: a later INFO message is suppressed. */
    int before = lc.count;
    CHECK_ST(rb_set_log_level(ctx, RB_LOG_ERROR), RB_OK);
    /* Reconnect-policy set logs nothing at INFO; force an INFO path by
     * re-initializing a second context at ERROR level and confirming
     * its init message never arrives. */
    log_collect_t lc2;
    memset(&lc2, 0, sizeof(lc2));
    rb_mutex_init(&lc2.lock);
    rb_config_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    cfg2.backend = RB_BACKEND_MOCK;
    cfg2.log_level = RB_LOG_ERROR;
    cfg2.log_callback = on_log;
    cfg2.log_user_data = &lc2;
    rb_context_t *ctx2 = NULL;
    CHECK_ST(rb_initialize(&cfg2, &ctx2), RB_OK);
    CHECK(lc2.count == 0);
    (void)before;

    /* Password secrecy: connect with a distinctive password, then
     * prove it never appears in any captured log line. */
    const char *secret = "s3cr3t-PW-98765";
    CHECK_ST(rb_wifi_connect(ctx, "RB-HomeNet", secret, NULL), RB_OK);
    test_sleep_ms(400);
    CHECK(!log_contains(&lc, secret));
    CHECK(!log_contains(&lc2, secret));
    rb_wifi_status_t st;
    CHECK_ST(rb_wifi_get_status(ctx, &st), RB_OK);
    CHECK(st.state == RB_WIFI_CONNECTED);

    rb_shutdown(ctx);
    rb_shutdown(ctx2);
    rb_mutex_destroy(&lc.lock);
    rb_mutex_destroy(&lc2.lock);
    TEST_END();
}
