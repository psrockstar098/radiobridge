/*
 * RadioBridge — rb_diagnostics.h
 *
 * Platform diagnostics + compatibility reporting (Phase 4). This is
 * additive: it reports what the active backend can see and what its
 * documented limits are. It never fails because a feature is missing
 * — missing information is reported as "not reported" / -1 fields,
 * matching the capability model's report-don't-fail pattern.
 *
 *   rb_get_diagnostics(ctx, out) — fills out from the live backend.
 *     backend_name/version/os_name always come from the capabilities
 *     snapshot; backend-specific fields come from the backend's
 *     diagnostics hook when it implements one.
 */
#ifndef RADIOBRIDGE_RB_DIAGNOSTICS_H
#define RADIOBRIDGE_RB_DIAGNOSTICS_H

#include "radiobridge/rb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rb_diagnostics {
    char backend_name[64];
    char backend_version[64];
    char os_name[64];
    /* D-Bus address the backend uses, or "none" (mock / non-D-Bus
     * backends). Test overrides (RB_DBUS_ADDRESS) show up here. */
    char bus_address[256];
    /* 1 = service present, 0 = absent, -1 = not applicable to this
     * backend. */
    int bluez_present;
    int nm_present;
    char nm_version[64]; /* "" when NetworkManager is absent */
    char adapter_path[256];
    char adapter_address[32];
    int adapter_powered; /* 1/0/-1 unknown or not applicable */
    char wifi_device_path[256];
    /* Human-readable compatibility report: the backend's documented
     * limits and behavior differences (e.g. synthetic GATT handles,
     * percent-not-dBm signal, poll-based notify). */
    char compatibility[1024];
    char detail[512];
} rb_diagnostics_t;

rb_status_t rb_get_diagnostics(rb_context_t *context,
                               rb_diagnostics_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_DIAGNOSTICS_H */
