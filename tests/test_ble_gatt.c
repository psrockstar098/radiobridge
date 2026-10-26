/* Mock BLE GATT: connect, discover, read/write, notify, RSSI. */
#include "test_common.h"

typedef struct {
    volatile int connected;
    volatile int failed;
    volatile int last_state;
    rb_status_t last_status;
    char last_message[256];
} conn_state_t;

static void on_conn(rb_context_t *ctx,
                    const rb_ble_connection_event_t *ev, void *ud) {
    (void)ctx;
    conn_state_t *s = (conn_state_t *)ud;
    s->last_state = (int)ev->state;
    s->last_status = ev->status;
    snprintf(s->last_message, sizeof(s->last_message), "%s", ev->message);
    if (ev->state == RB_BLE_CONNECTED) s->connected = 1;
    if (ev->state == RB_BLE_FAILED) s->failed = 1;
}

typedef struct {
    volatile int count;
    uint8_t last[RB_BLE_MAX_VALUE_LEN];
    size_t last_len;
} notify_collect_t;

static void on_notify(rb_context_t *ctx,
                      const rb_ble_notification_t *n, void *ud) {
    (void)ctx;
    notify_collect_t *c = (notify_collect_t *)ud;
    memcpy(c->last, n->data, n->data_len);
    c->last_len = n->data_len;
    c->count++;
}

static rb_device_id_t dev_id(const char *addr) {
    rb_device_id_t id;
    memset(&id, 0, sizeof(id));
    snprintf(id.address, sizeof(id.address), "%s", addr);
    return id;
}

int main(void) {
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_OK);

    rb_device_id_t hr = dev_id("AA:BB:CC:00:00:01");
    rb_device_id_t tag = dev_id("AA:BB:CC:00:00:03");
    rb_device_id_t ghost = dev_id("FF:FF:FF:FF:FF:FF");

    /* Argument validation. */
    CHECK_ST(rb_ble_connect(ctx, NULL, NULL, on_conn, NULL, NULL),
             RB_ERROR_INVALID_ARGUMENT);
    rb_operation_id_t dummy = 0;
    CHECK_ST(rb_ble_connect(ctx, &hr, NULL, NULL, NULL, &dummy),
             RB_ERROR_INVALID_ARGUMENT);

    /* Unknown device fails asynchronously with NOT_FOUND. */
    conn_state_t gs;
    memset(&gs, 0, sizeof(gs));
    rb_operation_id_t gop = 0;
    CHECK_ST(rb_ble_connect(ctx, &ghost, NULL, on_conn, &gs, &gop),
             RB_OK);
    CHECK(test_wait_until(&gs.failed, 2000));
    CHECK(gs.last_status == RB_ERROR_NOT_FOUND);

    /* Connect the heart-rate monitor. */
    conn_state_t hs;
    memset(&hs, 0, sizeof(hs));
    rb_operation_id_t hop = 0;
    CHECK_ST(rb_ble_connect(ctx, &hr, NULL, on_conn, &hs, &hop), RB_OK);
    CHECK(test_wait_until(&hs.connected, 2000));
    CHECK_ST(rb_ble_connect(ctx, &hr, NULL, on_conn, &hs, &hop),
             RB_ERROR_ALREADY_IN_PROGRESS);

    /* Discover: 2 services, expected characteristics + descriptor. */
    rb_ble_service_t *svcs = NULL;
    size_t nsvc = 0;
    CHECK_ST(rb_ble_discover(ctx, &hr, &svcs, &nsvc), RB_OK);
    CHECK(nsvc == 2);
    rb_uuid_t u180d, u2a37, u2a38, u2902;
    CHECK_ST(rb_uuid_from_16(0x180D, &u180d), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x2A37, &u2a37), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x2A38, &u2a38), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x2902, &u2902), RB_OK);
    int saw_hr = 0, saw_batt = 0;
    for (size_t i = 0; i < nsvc; i++) {
        if (rb_uuid_equals(&svcs[i].uuid, &u180d)) {
            saw_hr = 1;
            CHECK(svcs[i].characteristic_count == 2);
            int saw_meas = 0, saw_loc = 0;
            for (size_t j = 0; j < svcs[i].characteristic_count; j++) {
                rb_ble_characteristic_t *ch = &svcs[i].characteristics[j];
                if (rb_uuid_equals(&ch->uuid, &u2a37)) {
                    saw_meas = 1;
                    CHECK((ch->properties & RB_GATT_PROP_NOTIFY) != 0);
                    CHECK(ch->descriptor_count == 1);
                    CHECK(rb_uuid_equals(&ch->descriptors[0].uuid,
                                         &u2902));
                }
                if (rb_uuid_equals(&ch->uuid, &u2a38)) {
                    saw_loc = 1;
                    CHECK((ch->properties & RB_GATT_PROP_READ) != 0);
                }
            }
            CHECK(saw_meas && saw_loc);
        }
        rb_uuid_t u180f;
        CHECK_ST(rb_uuid_from_16(0x180F, &u180f), RB_OK);
        if (rb_uuid_equals(&svcs[i].uuid, &u180f)) saw_batt = 1;
    }
    CHECK(saw_hr && saw_batt);
    rb_free(svcs);

    /* Reads: battery 87, body location 1; notify-only read rejected. */
    rb_uuid_t u180f, u2a19;
    CHECK_ST(rb_uuid_from_16(0x180F, &u180f), RB_OK);
    CHECK_ST(rb_uuid_from_16(0x2A19, &u2a19), RB_OK);
    uint8_t buf[RB_BLE_MAX_VALUE_LEN];
    size_t len = 0;
    CHECK_ST(rb_ble_read(ctx, &hr, &u180f, &u2a19, buf, &len), RB_OK);
    CHECK(len == 1 && buf[0] == 87);
    CHECK_ST(rb_ble_read(ctx, &hr, &u180d, &u2a38, buf, &len), RB_OK);
    CHECK(len == 1 && buf[0] == 0x01);
    CHECK_ST(rb_ble_read(ctx, &hr, &u180d, &u2a37, buf, &len),
             RB_ERROR_NOT_SUPPORTED);
    /* Unknown characteristic. */
    rb_uuid_t u9999;
    CHECK_ST(rb_uuid_from_16(0x9999, &u9999), RB_OK);
    CHECK_ST(rb_ble_read(ctx, &hr, &u180d, &u9999, buf, &len),
             RB_ERROR_NOT_FOUND);

    /* Write to a read-only characteristic is rejected. */
    uint8_t wv[1] = {0x05};
    CHECK_ST(rb_ble_write(ctx, &hr, &u180f, &u2a19, wv, 1, 1),
             RB_ERROR_NOT_SUPPORTED);

    /* RSSI. */
    int8_t rssi = 0;
    CHECK_ST(rb_ble_get_rssi(ctx, &hr, &rssi), RB_OK);
    CHECK(rssi == -52);

    /* Notifications: subscribe to battery, receive data, unsubscribe. */
    notify_collect_t nc;
    memset(&nc, 0, sizeof(nc));
    CHECK_ST(rb_ble_subscribe(ctx, &hr, &u180f, &u2a19, on_notify, &nc),
             RB_OK);
    CHECK(test_wait_count(&nc.count, 1, 2000));
    CHECK(nc.last_len == 1 && nc.last[0] == 87);
    CHECK_ST(rb_ble_unsubscribe(ctx, &hr, &u180f, &u2a19), RB_OK);
    CHECK_ST(rb_ble_unsubscribe(ctx, &hr, &u180f, &u2a19),
             RB_ERROR_NOT_FOUND);
    /* Subscribing to a read-only characteristic is rejected. */
    CHECK_ST(rb_ble_subscribe(ctx, &hr, &u180d, &u2a38, on_notify, &nc),
             RB_ERROR_NOT_SUPPORTED);

    /* Echo round-trip on the demo tag. */
    conn_state_t ts;
    memset(&ts, 0, sizeof(ts));
    rb_operation_id_t top = 0;
    CHECK_ST(rb_ble_connect(ctx, &tag, NULL, on_conn, &ts, &top), RB_OK);
    CHECK(test_wait_until(&ts.connected, 2000));
    rb_uuid_t svc128, echo_uuid, fixed_uuid;
    CHECK_ST(rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345678",
                                 &svc128),
             RB_OK);
    CHECK_ST(rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345679",
                                 &echo_uuid),
             RB_OK);
    CHECK_ST(rb_uuid_from_string("12345678-1234-5678-9ABC-DEF012345680",
                                 &fixed_uuid),
             RB_OK);
    const char *msg = "hello-radio";
    CHECK_ST(rb_ble_write(ctx, &tag, &svc128, &echo_uuid,
                          (const uint8_t *)msg, strlen(msg), 1),
             RB_OK);
    len = 0;
    CHECK_ST(rb_ble_read(ctx, &tag, &svc128, &echo_uuid, buf, &len),
             RB_OK);
    CHECK(len == strlen(msg) && memcmp(buf, msg, len) == 0);
    /* Write-without-response also works on the echo characteristic. */
    CHECK_ST(rb_ble_write(ctx, &tag, &svc128, &echo_uuid,
                          (const uint8_t *)"v2", 2, 0),
             RB_OK);
    len = 0;
    CHECK_ST(rb_ble_read(ctx, &tag, &svc128, &echo_uuid, buf, &len),
             RB_OK);
    CHECK(len == 2 && memcmp(buf, "v2", 2) == 0);

    /* Disconnect paths. */
    CHECK_ST(rb_ble_disconnect(ctx, &hr), RB_OK);
    CHECK_ST(rb_ble_disconnect(ctx, &hr), RB_ERROR_NOT_CONNECTED);
    CHECK_ST(rb_ble_discover(ctx, &hr, &svcs, &nsvc),
             RB_ERROR_NOT_CONNECTED);
    CHECK_ST(rb_ble_read(ctx, &hr, &u180f, &u2a19, buf, &len),
             RB_ERROR_NOT_CONNECTED);
    CHECK_ST(rb_ble_disconnect(ctx, &tag), RB_OK);

    rb_shutdown(ctx);
    TEST_END();
}
