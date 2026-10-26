/* Error paths and argument validation across the public API. */
#include "test_common.h"

int main(void) {
    /* NULL context → NOT_INITIALIZED on every public entry point. */
    rb_capabilities_t caps;
    rb_wifi_status_t wst;
    rb_ble_service_t *svcs = NULL;
    size_t nsvc = 0;
    rb_operation_id_t op = 0;
    uint8_t buf[RB_BLE_MAX_VALUE_LEN];
    size_t len = 0;
    rb_device_id_t dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.address, sizeof(dev.address), "AA:BB:CC:00:00:01");
    rb_uuid_t u;
    CHECK_ST(rb_uuid_from_16(0x180D, &u), RB_OK);

    CHECK_ST(rb_get_capabilities(NULL, &caps), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_scan_start(NULL, NULL, NULL, NULL, &op),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_scan_stop(NULL, 1), RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_connect(NULL, &dev, NULL, NULL, NULL, &op),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_disconnect(NULL, &dev), RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_discover(NULL, &dev, &svcs, &nsvc),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_read(NULL, &dev, &u, &u, buf, &len),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_write(NULL, &dev, &u, &u, buf, 1, 1),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_subscribe(NULL, &dev, &u, &u, NULL, NULL),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_ble_unsubscribe(NULL, &dev, &u, &u),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_wifi_get_status(NULL, &wst), RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_wifi_connect(NULL, "x", NULL, NULL),
             RB_ERROR_NOT_INITIALIZED);
    CHECK_ST(rb_wifi_disconnect(NULL), RB_ERROR_NOT_INITIALIZED);

    /* Live context, bad arguments → INVALID_ARGUMENT. */
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    CHECK_ST(rb_ble_scan_start(ctx, NULL, NULL, NULL, &op),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_scan_start(ctx, NULL, NULL, NULL, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_scan_stop(ctx, 0), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_connect(ctx, NULL, NULL, NULL, NULL, &op),
             RB_ERROR_INVALID_ARGUMENT);
    rb_device_id_t empty_dev;
    memset(&empty_dev, 0, sizeof(empty_dev));
    CHECK_ST(rb_ble_connect(ctx, &empty_dev, NULL, NULL, NULL, &op),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_discover(ctx, &dev, NULL, &nsvc),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_discover(ctx, &dev, &svcs, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_read(ctx, &dev, &u, &u, NULL, &len),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_ble_write(ctx, &dev, &u, &u, NULL, 5, 1),
             RB_ERROR_INVALID_ARGUMENT);
    uint8_t big[RB_BLE_MAX_VALUE_LEN + 1];
    CHECK_ST(rb_ble_write(ctx, &dev, &u, &u, big,
                          sizeof(big), 1),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_wifi_connect(ctx, NULL, NULL, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_wifi_connect(ctx, "", NULL, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_wifi_get_status(ctx, NULL), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_wifi_measure_quality(ctx, NULL, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_get_required_permissions(ctx, (rb_operation_t)99, NULL,
                                         NULL, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_set_log_level(ctx, (rb_log_level_t)99),
             RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_set_log_level(NULL, RB_LOG_DEBUG),
             RB_ERROR_INVALID_ARGUMENT);

    /* UUID parsing. */
    rb_uuid_t a, b;
    CHECK_ST(rb_uuid_from_string("180D", &a), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x180D, &b), RB_OK);
    CHECK(rb_uuid_equals(&a, &b));
    CHECK_ST(rb_uuid_from_string(
                 "0000180D-0000-1000-8000-00805F9B34FB", &a),
             RB_OK);
    CHECK(rb_uuid_equals(&a, &b));
    CHECK_ST(rb_uuid_from_string("ZZZ", &a), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_uuid_from_string("180", &a), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_uuid_from_string(NULL, &a), RB_ERROR_INVALID_ARGUMENT);
    CHECK_ST(rb_uuid_from_string("180D", NULL), RB_ERROR_INVALID_ARGUMENT);
    char ustr[40];
    rb_uuid_to_string(&b, ustr, sizeof(ustr));
    CHECK(strcmp(ustr, "0000180d-0000-1000-8000-00805f9b34fb") == 0);
    CHECK(!rb_uuid_equals(NULL, &b));
    CHECK(!rb_uuid_equals(&b, NULL));

    /* Status strings never NULL. */
    CHECK(rb_status_string(RB_OK) != NULL);
    CHECK(rb_status_string(RB_ERROR_NOT_FOUND) != NULL);
    CHECK(rb_status_string((rb_status_t)999) != NULL);

    /* rb_free is safe on NULL. */
    rb_free(NULL);

    rb_shutdown(ctx);
    TEST_END();
}
