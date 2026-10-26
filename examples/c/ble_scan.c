/*
 * RadioBridge example: BLE scan on the mock backend.
 *
 * Build: cmake --preset default && cmake --build build -j1
 * Run:   ./build/examples/example_ble_scan
 *
 * Prints the mock catalog (3 fake devices) and the capabilities of the
 * active backend. Replace RB_BACKEND_MOCK with RB_BACKEND_AUTO on a
 * machine with real radios once the native backends land.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "radiobridge/rb.h"

static void on_scan(rb_context_t *ctx, const rb_ble_device_t *dev,
                    void *ud) {
    (void)ctx;
    (void)ud;
    printf("  %-16s %s  RSSI %d dBm  services: %zu\n", dev->address,
           dev->name, (int)dev->rssi_dbm, dev->service_uuid_count);
}

static void sleep_ms(unsigned int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int main(void) {
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    rb_context_t *ctx = NULL;
    rb_status_t st = rb_initialize(&cfg, &ctx);
    if (st != RB_OK) {
        fprintf(stderr, "init failed: %s\n", rb_status_string(st));
        return 1;
    }
    printf("RadioBridge %s\n", rb_version_string());
    rb_capabilities_t caps;
    rb_get_capabilities(ctx, &caps);
    printf("backend: %s (%s)\n", caps.backend_name, caps.detail);

    rb_ble_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.manufacturer_id = -1;
    rb_operation_id_t op = 0;
    st = rb_ble_scan_start(ctx, &opts, on_scan, NULL, &op);
    if (st != RB_OK) {
        fprintf(stderr, "scan failed: %s\n", rb_status_string(st));
        rb_shutdown(ctx);
        return 1;
    }
    printf("scanning...\n");
    sleep_ms(1200);
    rb_ble_scan_stop(ctx, op);
    rb_shutdown(ctx);
    return 0;
}
