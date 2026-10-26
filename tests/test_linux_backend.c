/* Linux backend: probe honesty and clean unavailable behavior.
 * This test asserts the Phase 1/2 contract: on a machine without the
 * platform services, the backend says so and fabricates nothing. The
 * probe assertions (all UNAVAILABLE + non-empty detail) hold on every
 * machine in this phase, because no D-Bus transport exists yet. */
#include "core/rb_internal.h"
#include "test_common.h"

int main(void) {
    rb_capabilities_t caps;
    CHECK_ST(rb_linux_probe(&caps), RB_OK);
    CHECK(strcmp(caps.backend_name, "linux") == 0);
    CHECK(caps.ble.scan == RB_CAP_UNAVAILABLE);
    CHECK(caps.ble.connect == RB_CAP_UNAVAILABLE);
    CHECK(caps.ble.gatt == RB_CAP_UNAVAILABLE);
    CHECK(caps.ble.notify == RB_CAP_UNAVAILABLE);
    CHECK(caps.ble.pairing == RB_CAP_UNAVAILABLE);
    CHECK(caps.ble.rssi == RB_CAP_UNAVAILABLE);
    CHECK(caps.ble.reconnect == RB_CAP_UNAVAILABLE);
    CHECK(caps.wifi.status == RB_CAP_UNAVAILABLE);
    CHECK(caps.wifi.scan == RB_CAP_UNAVAILABLE);
    CHECK(caps.wifi.connect == RB_CAP_UNAVAILABLE);
    CHECK(caps.wifi.quality == RB_CAP_UNAVAILABLE);
    CHECK(caps.wifi.saved_networks == RB_CAP_UNAVAILABLE);
    CHECK(caps.detail[0] != '\0');
    /* The detail names the probed evidence paths. */
    CHECK(strstr(caps.detail, "/sys/class/bluetooth") != NULL);
    CHECK(strstr(caps.detail, "/run/dbus/system_bus_socket") != NULL);
    CHECK(strstr(caps.detail, "/run/NetworkManager") != NULL);

    /* On this VM every service is absent. */
    CHECK(strstr(caps.detail, "absent") != NULL);

    /* Explicit init fails fast; no context, no fake devices. */
    rb_context_t *ctx = NULL;
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_LINUX;
    cfg.log_level = RB_LOG_WARN;
    CHECK_ST(rb_initialize(&cfg, &ctx), RB_ERROR_UNAVAILABLE);
    CHECK(ctx == NULL);

    TEST_END();
}
