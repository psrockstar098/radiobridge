/*
 * RadioBridge — rb.h
 *
 * Umbrella header + core lifecycle: context creation, capabilities,
 * cancellation, permissions, logging, and memory helpers.
 *
 * LIFECYCLE:
 *   rb_initialize(&config, &ctx) -> ... -> rb_shutdown(ctx)
 * A context owns its backend, its worker threads, and all in-flight
 * operations. rb_shutdown() stops every operation, joins worker threads,
 * and frees the context; the pointer must not be used afterwards.
 *
 * THREAD SAFETY:
 * All public functions are safe to call from any thread. Callbacks fire
 * from internal threads and must themselves be thread-safe. Never call
 * rb_shutdown() from inside a callback.
 *
 * CANCELLATION:
 * Long-running operations (scans, connects) receive an operation id.
 * rb_cancel() requests cancellation; the backend stops the work and the
 * operation's callback (if any) receives a final CANCELLED/FAILED event
 * where the API defines one. Cancellation is cooperative and prompt on
 * the mock backend (checked between deliveries); native backends map it
 * to their platform cancel where one exists and otherwise stop at the
 * next safe point (documented per backend — e.g. WinRT cannot cancel an
 * in-flight connection attempt, so it completes or times out).
 */
#ifndef RADIOBRIDGE_RB_H
#define RADIOBRIDGE_RB_H

#include "rb_errors.h"
#include "rb_types.h"
#include "rb_bluetooth.h"
#include "rb_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a context. `config` may be NULL for defaults (AUTO backend,
 * WARN logging, stderr logger, no event callback). On success
 * *out_context owns the context; call rb_shutdown() exactly once.
 * RB_BACKEND_AUTO prefers the native backend for the host and falls
 * back to mock (with a warning log) when native is unavailable.
 * An explicitly requested backend that is unavailable fails here with
 * RB_ERROR_UNAVAILABLE. */
rb_status_t rb_initialize(const rb_config_t *config,
                          rb_context_t **out_context);

/* Destroy a context: stops all operations, joins worker threads, frees
 * memory. Safe to call with NULL (no-op). After this call the context
 * pointer is invalid. */
void rb_shutdown(rb_context_t *context);

/* Report the active backend's capabilities. Always succeeds on a live
 * context. Callers MUST check the capability states before calling an
 * operation — never assume a feature exists because another backend
 * offers it. */
rb_status_t rb_get_capabilities(rb_context_t *context,
                                rb_capabilities_t *out_caps);

/* Cancel an in-flight operation by id. Returns RB_ERROR_NOT_FOUND for
 * unknown or already-finished ids. Cancellation is best-effort on
 * native backends where the platform lacks a cancel primitive. */
rb_status_t rb_cancel(rb_context_t *context, rb_operation_id_t operation);

/* Report the OS permissions an operation requires on the active
 * backend, as a bitmask (RB_PERM_*) and, when `out_names` is non-NULL,
 * as short names ("BLUETOOTH_SCAN", ...). *out_count is both input
 * (capacity of out_names) and output (names written). Pass NULL names
 * with *out_count = 0 to query the count. The library never requests
 * permissions itself. */
rb_status_t rb_get_required_permissions(rb_context_t *context,
                                        rb_operation_t operation,
                                        rb_permission_mask_t *out_mask,
                                        char (*out_names)[32],
                                        size_t *out_count);

/* Runtime version strings. rb_version_string() returns e.g. "0.1.0". */
const char *rb_version_string(void);
uint32_t rb_abi_version(void);

/* Change the log level at runtime (default comes from rb_config_t). */
rb_status_t rb_set_log_level(rb_context_t *context, rb_log_level_t level);

/* Free a heap array previously returned by rb_ble_discover() or a list
 * helper. Safe with NULL. */
void rb_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_H */
