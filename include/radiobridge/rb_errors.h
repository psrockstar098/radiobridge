/*
 * RadioBridge — rb_errors.h
 *
 * Stable error taxonomy for the public C API. Every function that can fail
 * returns one of these codes; rb_status_string() maps each code to a stable,
 * human-readable string. Backends translate their native errors into this
 * taxonomy (see docs/backends.md for the per-platform mapping tables).
 * New codes are appended only — never renumbered — to keep the ABI stable.
 */
#ifndef RADIOBRIDGE_RB_ERRORS_H
#define RADIOBRIDGE_RB_ERRORS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rb_status {
    RB_OK = 0,
    RB_ERROR_INVALID_ARGUMENT = 1, /* NULL pointer, bad enum, malformed id */
    RB_ERROR_NOT_INITIALIZED = 2,  /* context is NULL or already shut down */
    RB_ERROR_NO_MEMORY = 3,
    RB_ERROR_NOT_SUPPORTED = 4,    /* operation not supported by this backend */
    RB_ERROR_UNAVAILABLE = 5,      /* backend/service absent (e.g. no BlueZ) */
    RB_ERROR_PERMISSION_DENIED = 6,/* OS permission missing or denied */
    RB_ERROR_NOT_FOUND = 7,        /* device, network, service, char unknown */
    RB_ERROR_TIMEOUT = 8,
    RB_ERROR_CANCELLED = 9,        /* operation cancelled via rb_cancel() */
    RB_ERROR_ALREADY_IN_PROGRESS = 10,
    RB_ERROR_NOT_CONNECTED = 11,
    RB_ERROR_BUSY = 12,
    RB_ERROR_AUTH_FAILED = 13,     /* pairing / Wi-Fi authentication failed */
    RB_ERROR_INTERNAL = 14         /* backend bug or unexpected native error */
} rb_status_t;

/* Stable human-readable string for a status code. Never returns NULL. */
const char *rb_status_string(rb_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_ERRORS_H */
