/*
 * RadioBridge — rb_dbus.h
 *
 * Minimal libdbus transport wrapper for the Linux backend. Linux-only;
 * compiled only when libdbus is found (RB_HAVE_DBUS). This file wraps
 * the repetitive libdbus mechanics — connection setup, method calls,
 * error mapping, and reply parsing — so rb_linux.c stays focused on
 * the BlueZ / NetworkManager interface logic.
 *
 * Bus-address resolution order (documented, testable):
 *   1. RB_DBUS_ADDRESS environment variable (test override).
 *   2. DBUS_SYSTEM_BUS_ADDRESS environment variable.
 *   3. The default system bus.
 *
 * Threading: a connection is used from multiple threads (scan/connect
 * workers, notify poll threads) but every method call is serialized
 * through the backend mutex in rb_linux.c. libdbus itself is used only
 * via blocking calls (send_with_reply_and_block); there is no dispatch
 * thread, so no libdbus threading hazard exists.
 */
#ifndef RADIOBRIDGE_RB_DBUS_H
#define RADIOBRIDGE_RB_DBUS_H

#include <stddef.h>
#include <stdint.h>

#include <dbus/dbus.h>

#include "radiobridge/rb_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rb_dbus_conn rb_dbus_conn_t;

/* Open a bus connection. On failure returns a non-OK status and fills
 * `detail` (never NULL when detail_len > 0). */
rb_status_t rb_dbus_open(rb_dbus_conn_t **out, char *detail,
                         size_t detail_len);
void rb_dbus_close(rb_dbus_conn_t *conn);

/* The address this connection was opened on (for diagnostics). */
const char *rb_dbus_address(rb_dbus_conn_t *conn);

/* 1 if the well-known name has an owner on this bus, 0 if not,
 * -1 on transport error (detail filled). */
int rb_dbus_name_has_owner(rb_dbus_conn_t *conn, const char *name,
                           char *detail, size_t detail_len);

/* Create a method-call message (caller owns it until passed to
 * rb_dbus_call, which unrefs it in all paths). */
DBusMessage *rb_dbus_new_call(const char *dest, const char *path,
                              const char *iface, const char *method);

/* Append helpers for method-call arguments. */
void rb_dbus_append_string(DBusMessage *msg, const char *value);
void rb_dbus_append_object_path(DBusMessage *msg, const char *value);
void rb_dbus_append_bool(DBusMessage *msg, int value);
void rb_dbus_append_uint32(DBusMessage *msg, uint32_t value);
void rb_dbus_append_int16(DBusMessage *msg, int16_t value);
void rb_dbus_append_byte_array(DBusMessage *msg, const uint8_t *bytes,
                               size_t len);
/* Container helpers. libdbus requires a container to be closed through
 * the exact parent iterator that opened it, so callers own the append
 * iterator and pass it to both open and close (LIFO order). */
/* Initialize an append iterator on a message. */
void rb_dbus_append_iter(DBusMessage *msg, DBusMessageIter *it);
/* Open an a{sv} dictionary on `parent`; close with rb_dbus_close_dict. */
void rb_dbus_open_dict(DBusMessageIter *parent, DBusMessageIter *dict);
void rb_dbus_close_dict(DBusMessageIter *parent, DBusMessageIter *dict);
/* Open an a{sa{sv}} NetworkManager settings dictionary on `parent`. */
void rb_dbus_open_settings(DBusMessageIter *parent, DBusMessageIter *outer);
void rb_dbus_close_settings(DBusMessageIter *parent,
                            DBusMessageIter *outer);
/* One setting inside an a{sa{sv}} dictionary. The struct keeps the
 * entry/dict iterators so close can unwind them in order. NOTE: the
 * section value is a PLAIN a{sv} array, not a variant — an earlier
 * version wrapped it in a variant and libdbus aborted the process
 * on the type mismatch. */
typedef struct rb_dbus_setting {
    DBusMessageIter entry;
    DBusMessageIter dict;
} rb_dbus_setting_t;
void rb_dbus_setting_open(DBusMessageIter *outer, rb_dbus_setting_t *s,
                          const char *name);
void rb_dbus_setting_close(DBusMessageIter *outer, rb_dbus_setting_t *s);
/* Add one entry to an open a{sv} dictionary iterator. */
void rb_dbus_dict_put_string(DBusMessageIter *dict, const char *key,
                             const char *value);
void rb_dbus_dict_put_bool(DBusMessageIter *dict, const char *key,
                           int value);
void rb_dbus_dict_put_uint32(DBusMessageIter *dict, const char *key,
                             uint32_t value);
void rb_dbus_dict_put_byte_array(DBusMessageIter *dict, const char *key,
                                 const uint8_t *bytes, size_t len);

/* Send a method call and block for the reply (timeout_ms; 0 = 5000).
 * Returns the reply message (caller unrefs) or NULL on error, with
 * `err_name` set to the D-Bus error name (e.g.
 * "org.freedesktop.DBus.Error.ServiceUnknown", "timeout",
 * "disconnected") and `detail` filled. The request message is unref'd
 * in all paths. */
DBusMessage *rb_dbus_call(rb_dbus_conn_t *conn, DBusMessage *request,
                          uint32_t timeout_ms, char *err_name,
                          size_t err_name_len, char *detail,
                          size_t detail_len);

/* Convenience: org.freedesktop.DBus.Properties.Get. Returns the reply
 * (single variant argument) or NULL on error (err_name/detail filled). */
DBusMessage *rb_dbus_get_property(rb_dbus_conn_t *conn, const char *dest,
                                  const char *path, const char *iface,
                                  const char *prop, char *err_name,
                                  size_t err_name_len, char *detail,
                                  size_t detail_len);

/* Map a D-Bus error name to the closest rb_status_t. */
rb_status_t rb_dbus_error_to_status(const char *err_name);

/* --- Reply parsing -------------------------------------------------- */
/* Each variant_* helper reads the FIRST argument of `reply`, which must
 * be a variant, and extracts the typed value inside it. Returns 1 on
 * success, 0 on type mismatch / missing argument. */
int rb_dbus_variant_string(DBusMessage *reply, char *out, size_t out_len);
int rb_dbus_variant_int16(DBusMessage *reply, int16_t *out);
int rb_dbus_variant_uint32(DBusMessage *reply, uint32_t *out);
int rb_dbus_variant_bool(DBusMessage *reply, int *out);
int rb_dbus_variant_object_path(DBusMessage *reply, char *out,
                                size_t out_len);
int rb_dbus_variant_byte_array(DBusMessage *reply, uint8_t *out,
                               size_t cap, size_t *out_len);
/* First argument must be an array of object paths. Calls cb once per
 * path. */
void rb_dbus_reply_each_object_path(
    DBusMessage *reply, void (*cb)(const char *path, void *ud), void *ud);

/* Find `key` in an a{sv} dictionary iterator (positioned at its first
 * entry). On success returns 1 and `val` holds an iterator over the
 * variant's INNER value (already recursed). Returns 0 when absent. */
int rb_dbus_dict_find(DBusMessageIter *dict, const char *key,
                      DBusMessageIter *val);
/* Typed getters on an already-recursed variant inner iterator. */
int rb_dbus_iter_string(DBusMessageIter *it, char *out, size_t out_len);
int rb_dbus_iter_int16(DBusMessageIter *it, int16_t *out);
int rb_dbus_iter_uint32(DBusMessageIter *it, uint32_t *out);
int rb_dbus_iter_bool(DBusMessageIter *it, int *out);
int rb_dbus_iter_object_path(DBusMessageIter *it, char *out,
                             size_t out_len);
int rb_dbus_iter_byte_array(DBusMessageIter *it, uint8_t *out, size_t cap,
                            size_t *out_len);
/* Call cb once per string in an `as` array iterator. */
void rb_dbus_iter_each_string(DBusMessageIter *it,
                              void (*cb)(const char *s, void *ud),
                              void *ud);
/* Call cb once per entry of an a{qay} dictionary iterator
 * (ManufacturerData: uint16 id -> byte array). */
void rb_dbus_iter_each_manufacturer(
    DBusMessageIter *it,
    void (*cb)(uint16_t id, const uint8_t *data, size_t len, void *ud),
    void *ud);

/* Walk a GetManagedObjects reply (a{oa{sa{sv}}}). For every
 * (object path, interface) pair, cb is invoked with an iterator over
 * that interface's a{sv} property dictionary. */
void rb_dbus_walk_managed_objects(
    DBusMessage *reply,
    void (*cb)(const char *path, const char *iface, DBusMessageIter *props,
               void *ud),
    void *ud);

#ifdef __cplusplus
}
#endif

#endif /* RADIOBRIDGE_RB_DBUS_H */
