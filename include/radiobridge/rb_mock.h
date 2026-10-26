/*
 * RadioBridge — rb_mock.h
 *
 * TEST-ONLY hooks for the mock backend. These are NOT part of the stable
 * ABI: they exist so tests (and demo apps) can drive mock-backend
 * scenarios — simulated drops, reconnect-policy verification — that
 * have no meaning on real radios. They return RB_ERROR_NOT_SUPPORTED
 * on any non-mock backend. Do not ship production code against them.
 */
#ifndef RADIOBRIDGE_RB_MOCK_H
#define RADIOBRIDGE_RB_MOCK_H

#include "rb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Simulate a radio drop for a connected mock device: the backend fires
 * a DISCONNECTED event, then applies the configured reconnect policy
 * (reconnect attempts surface through the connection callback). */
rb_status_t rb_mock_trigger_disconnect(rb_context_t *context,
                                       const rb_device_id_t *device);

/* Simulate a Wi-Fi state change (used to test RB_EVENT_WIFI_CHANGED
 * delivery without touching real interfaces). */
rb_status_t rb_mock_trigger_wifi_change(rb_context_t *context,
                                        rb_wifi_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_MOCK_H */
