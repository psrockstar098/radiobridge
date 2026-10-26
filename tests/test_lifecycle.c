/* Lifecycle: init/shutdown, defaults, bad arguments, version. */
#include "test_common.h"

int main(void) {
    rb_context_t *ctx = NULL;

    CHECK_ST(rb_initialize(NULL, NULL), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_initialize(NULL, &ctx), RB_OK);
    CHECK(ctx != NULL);

    /* Defaults: AUTO falls back to mock on this VM (no native radio). */
    rb_capabilities_t caps;
    CHECK_ST(rb_get_capabilities(ctx, &caps), RB_OK);
    CHECK(strcmp(caps.backend_name, "mock") == 0);

    /* Version surface. */
    CHECK(strcmp(rb_version_string(), "0.2.0") == 0);
    CHECK(rb_abi_version() == RB_API_VERSION);
    CHECK_ST(rb_get_capabilities(NULL, &caps), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_get_capabilities(ctx, NULL), RB_ERROR_INVALID_ARGUMENT);

    /* Bad backend kind. */
    rb_context_t *bad = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = (rb_backend_kind_t)99;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &bad), RB_ERROR_INVALID_ARGUMENT);
    CHECK(bad == NULL);

    /* Explicit mock works. */
    rb_context_t *mctx = NULL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &mctx), RB_OK);
    CHECK_ST(rb_get_capabilities(mctx, &caps), RB_OK);
    CHECK(strcmp(caps.backend_name, "mock") == 0);

    /* Shutdown is a no-op on NULL and frees cleanly. */
    rb_shutdown(NULL);
    rb_shutdown(mctx);
    rb_shutdown(ctx);
    TEST_END();
}
