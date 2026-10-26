/* Permission reporting: masks and names per operation. */
#include "test_common.h"

static int mask_has_name(rb_context_t *ctx, rb_operation_t op,
                         const char *name) {
    char names[8][32];
    size_t count = 8;
    rb_permission_mask_t mask = 0;
    if (rb_get_required_permissions(ctx, op, &mask, names, &count) !=
        RB_OK)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) return 1;
    }
    return 0;
}

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    rb_permission_mask_t mask = 0;
    size_t count = 0;

    /* Count query: NULL names still reports how many names exist. */
    CHECK_ST(rb_get_required_permissions(ctx, RB_OP_BLE_SCAN, &mask,
                                         NULL, &count),
             RB_OK);
    CHECK(count == 2);
    CHECK((mask & RB_PERM_BLUETOOTH_SCAN) != 0);
    CHECK((mask & RB_PERM_LOCATION) != 0);

    CHECK(mask_has_name(ctx, RB_OP_BLE_SCAN, "BLUETOOTH_SCAN"));
    CHECK(mask_has_name(ctx, RB_OP_BLE_SCAN, "LOCATION"));
    CHECK(mask_has_name(ctx, RB_OP_BLE_CONNECT, "BLUETOOTH_CONNECT"));
    CHECK(mask_has_name(ctx, RB_OP_BLE_READ, "BLUETOOTH_CONNECT"));
    CHECK(mask_has_name(ctx, RB_OP_BLE_WRITE, "BLUETOOTH_CONNECT"));
    CHECK(mask_has_name(ctx, RB_OP_BLE_NOTIFY, "BLUETOOTH_CONNECT"));
    CHECK(mask_has_name(ctx, RB_OP_WIFI_SCAN, "WIFI_SCAN"));
    CHECK(mask_has_name(ctx, RB_OP_WIFI_CONNECT, "WIFI_CONNECT"));

    /* Status and quality need no permissions. */
    count = 8;
    CHECK_ST(rb_get_required_permissions(ctx, RB_OP_WIFI_STATUS, &mask,
                                         NULL, &count),
             RB_OK);
    CHECK(mask == RB_PERM_NONE);
    CHECK(count == 0);
    count = 8;
    CHECK_ST(rb_get_required_permissions(ctx, RB_OP_WIFI_QUALITY, &mask,
                                         NULL, &count),
             RB_OK);
    CHECK(mask == RB_PERM_NONE);

    /* Invalid operation and NULL outputs. */
    count = 8;
    CHECK_ST(rb_get_required_permissions(ctx, (rb_operation_t)42, &mask,
                                         NULL, &count),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_get_required_permissions(ctx, RB_OP_BLE_SCAN, NULL, NULL,
                                         &count),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_get_required_permissions(ctx, RB_OP_BLE_SCAN, &mask, NULL,
                                         NULL),
             RB_ERROR_INVALID_ARGUMENT);

    rb_shutdown(ctx);
    TEST_END();
}
