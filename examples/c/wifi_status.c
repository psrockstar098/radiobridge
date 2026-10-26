/*
 * RadioBridge example: Wi-Fi status + scan on the mock backend.
 *
 * Build: cmake --preset default && cmake --build build -j1
 * Run:   ./build/examples/example_wifi_status
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "radiobridge/rb.h"

static void on_net(rb_context_t *ctx, const rb_wifi_network_t *net,
                   void *ud) {
    (void)ctx;
    (void)ud;
    printf("  %-16s %s  %d dBm  %u MHz\n", net->ssid, net->bssid,
           (int)net->signal_dbm, net->frequency_mhz);
}

static void sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(void) {
    rb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = RB_BACKEND_MOCK;
    cfg.log_level = RB_LOG_WARN;
    rb_context_t *ctx = NULL;
    if (rb_initialize(&cfg, &ctx) != RB_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    rb_wifi_status_t st;
    rb_wifi_get_status(ctx, &st);
    printf("Wi-Fi state: %d (%s)\n", (int)st.state, st.message);

    rb_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    rb_operation_id_t op = 0;
    if (rb_wifi_scan_start(ctx, &opts, on_net, NULL, &op) != RB_OK) {
        fprintf(stderr, "scan failed\n");
        rb_shutdown(ctx);
        return 1;
    }
    printf("scanning...\n");
    sleep_ms(800);
    rb_wifi_scan_stop(ctx, op);

    rb_wifi_connect(ctx, "RB-CoffeeShop", NULL, NULL);
    sleep_ms(400);
    rb_wifi_get_status(ctx, &st);
    printf("after connect: %s @ %s (%s)\n", st.ssid, st.ip_address,
           st.message);
    rb_wifi_quality_t q;
    if (rb_wifi_measure_quality(ctx, NULL, &q) == RB_OK) {
        printf("quality: reachable=%d latency=%.1f ms loss=%.1f%%\n",
               q.reachable, q.latency_ms, q.packet_loss_percent);
    }
    rb_shutdown(ctx);
    return 0;
}
