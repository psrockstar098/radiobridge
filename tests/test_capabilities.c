/* Capabilities: mock surface, linux probe honesty, stub failures. */
#include "core/rb_internal.h"
#include "test_common.h"

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    rb_capabilities_t caps;
    CHECK_ST(rb_get_capabilities(ctx, &caps), RB_OK);
    CHECK(strcmp(caps.backend_name, "mock") == 0);
    CHECK(caps.ble.scan == RB_CAP_AVAILABLE);
    CHECK(caps.ble.connect == RB_CAP_AVAILABLE);
    CHECK(caps.ble.gatt == RB_CAP_AVAILABLE);
    CHECK(caps.ble.notify == RB_CAP_AVAILABLE);
    CHECK(caps.ble.rssi == RB_CAP_AVAILABLE);
    CHECK(caps.ble.reconnect == RB_CAP_AVAILABLE);
    /* No pairing API in the Phase 1 surface: must not claim it. */
    CHECK(caps.ble.pairing == RB_CAP_NOT_SUPPORTED);
    CHECK(caps.wifi.status == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.scan == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.connect == RB_CAP_AVAILABLE);
    CHECK(caps.wifi.quality == RB_CAP_AVAILABLE);
    /* No saved-network API in the Phase 1 surface. */
    CHECK(caps.wifi.saved_networks == RB_CAP_NOT_SUPPORTED);
    CHECK(caps.detail[0] != '\0');

    /* Linux probe: everything UNAVAILABLE in this phase, with a
     * diagnostic detail — never a fabricated "available". */
    rb_capabilities_t lcaps;
    CHECK_ST(rb_linux_probe(&lcaps), RB_OK);
    CHECK(strcmp(lcaps.backend_name, "linux") == 0);
    CHECK(lcaps.ble.scan == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.ble.connect == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.ble.gatt == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.ble.notify == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.wifi.status == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.wifi.scan == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.wifi.connect == RB_CAP_UNAVAILABLE);
    CHECK(lcaps.detail[0] != '\0');
    /* On this VM every service is absent; the detail must say so. */
    CHECK(strstr(lcaps.detail, "absent") != NULL);

    /* Explicit linux init fails fast and cleanly. */
    rb_context_t *lctx = NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_LINUX;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &lctx), RB_ERROR_UNAVAILABLE);
    CHECK(lctx == NULL);

    /* Explicit platform stubs fail with NOT_SUPPORTED, not a crash. */
    rb_context_t *actx = NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_ANDROID;
    CHECK_ST(rb_initialize(&cfg, &actx), RB_ERROR_NOT_SUPPORTED);
    rb_context_t *apctx = NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_APPLE;
    CHECK_ST(rb_initialize(&cfg, &apctx), RB_ERROR_NOT_SUPPORTED);
    rb_context_t *wctx = NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_WINDOWS;
    CHECK_ST(rb_initialize(&cfg, &wctx), RB_ERROR_NOT_SUPPORTED);

    rb_shutdown(ctx);
    TEST_END();
}
