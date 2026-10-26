/*
 * RadioBridge — rb_dbus.c
 *
 * libdbus transport wrapper. See rb_dbus.h. All D-Bus errors are
 * surfaced to the caller as (error name, detail) pairs so the backend
 * can map them to rb_status_t without losing the native reason.
 */
#include "rb_dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rb_dbus_conn {
    DBusConnection *conn;
    char address[512];
};

static void copy_str(char *dst, size_t len, const char *src) {
    if (len == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= len) n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

rb_status_t rb_dbus_open(rb_dbus_conn_t **out, char *detail,
                         size_t detail_len) {
    if (!out) return RB_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    const char *addr = getenv("RB_DBUS_ADDRESS");
    if (!addr || !*addr) addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = NULL;
    char addr_buf[512];
    if (addr && *addr) {
        copy_str(addr_buf, sizeof(addr_buf), addr);
        conn = dbus_connection_open_private(addr_buf, &err);
        if (!conn) {
            if (detail && detail_len)
                snprintf(detail, detail_len, "cannot open bus %s: %s",
                         addr_buf, err.message ? err.message : err.name);
            dbus_error_free(&err);
            return RB_ERROR_UNAVAILABLE;
        }
        if (!dbus_bus_register(conn, &err)) {
            if (detail && detail_len)
                snprintf(detail, detail_len,
                         "cannot register on bus %s: %s", addr_buf,
                         err.message ? err.message : err.name);
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            dbus_error_free(&err);
            return RB_ERROR_UNAVAILABLE;
        }
    } else {
        copy_str(addr_buf, sizeof(addr_buf), "system");
        conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (!conn) {
            if (detail && detail_len)
                snprintf(detail, detail_len, "cannot open system bus: %s",
                         err.message ? err.message : err.name);
            dbus_error_free(&err);
            return RB_ERROR_UNAVAILABLE;
        }
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    rb_dbus_conn_t *c = (rb_dbus_conn_t *)calloc(1, sizeof(*c));
    if (!c) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return RB_ERROR_NO_MEMORY;
    }
    c->conn = conn;
    copy_str(c->address, sizeof(c->address), addr_buf);
    *out = c;
    return RB_OK;
}

void rb_dbus_close(rb_dbus_conn_t *conn) {
    if (!conn) return;
    dbus_connection_close(conn->conn);
    dbus_connection_unref(conn->conn);
    free(conn);
}

const char *rb_dbus_address(rb_dbus_conn_t *conn) {
    return conn ? conn->address : "none";
}

rb_status_t rb_dbus_error_to_status(const char *err_name) {
    if (!err_name || !*err_name) return RB_ERROR_INTERNAL;
    if (strcmp(err_name, "timeout") == 0 ||
        strcmp(err_name, "org.freedesktop.DBus.Error.NoReply") == 0 ||
        strcmp(err_name, "org.freedesktop.DBus.Error.Timeout") == 0)
        return RB_ERROR_TIMEOUT;
    if (strcmp(err_name, "disconnected") == 0 ||
        strcmp(err_name, "org.freedesktop.DBus.Error.ServiceUnknown") == 0 ||
        strcmp(err_name, "org.freedesktop.DBus.Error.NameHasNoOwner") == 0 ||
        strcmp(err_name, "org.bluez.Error.NotReady") == 0 ||
        strcmp(err_name, "org.bluez.Error.NotAvailable") == 0)
        return RB_ERROR_UNAVAILABLE;
    if (strcmp(err_name, "org.bluez.Error.InProgress") == 0)
        return RB_ERROR_ALREADY_IN_PROGRESS;
    if (strcmp(err_name, "org.bluez.Error.NotConnected") == 0)
        return RB_ERROR_NOT_CONNECTED;
    if (strcmp(err_name, "org.bluez.Error.NotPaired") == 0 ||
        strcmp(err_name, "org.bluez.Error.AuthenticationFailed") == 0 ||
        strcmp(err_name, "org.bluez.Error.AuthenticationCanceled") == 0 ||
        strcmp(err_name, "org.bluez.Error.AuthenticationRejected") == 0)
        return RB_ERROR_AUTH_FAILED;
    if (strcmp(err_name, "org.bluez.Error.NotSupported") == 0 ||
        strcmp(err_name, "org.bluez.Error.UnsupportedMajorClass") == 0 ||
        strcmp(err_name, "org.bluez.Error.UnsupportedMinorClass") == 0)
        return RB_ERROR_NOT_SUPPORTED;
    if (strcmp(err_name, "org.bluez.Error.DoesNotExist") == 0)
        return RB_ERROR_NOT_FOUND;
    if (strcmp(err_name, "org.bluez.Error.InvalidArguments") == 0 ||
        strcmp(err_name, "org.freedesktop.DBus.Error.InvalidArgs") == 0)
        return RB_ERROR_INVALID_ARGUMENT;
    if (strcmp(err_name, "org.freedesktop.DBus.Error.AccessDenied") == 0)
        return RB_ERROR_PERMISSION_DENIED;
    return RB_ERROR_INTERNAL;
}

DBusMessage *rb_dbus_new_call(const char *dest, const char *path,
                              const char *iface, const char *method) {
    return dbus_message_new_method_call(dest, path, iface, method);
}

void rb_dbus_append_string(DBusMessage *msg, const char *value) {
    const char *v = value ? value : "";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &v, DBUS_TYPE_INVALID);
}

void rb_dbus_append_object_path(DBusMessage *msg, const char *value) {
    const char *v = value ? value : "/";
    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &v,
                             DBUS_TYPE_INVALID);
}

void rb_dbus_append_bool(DBusMessage *msg, int value) {
    dbus_bool_t v = value ? TRUE : FALSE;
    dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &v, DBUS_TYPE_INVALID);
}

void rb_dbus_append_uint32(DBusMessage *msg, uint32_t value) {
    dbus_message_append_args(msg, DBUS_TYPE_UINT32, &value,
                             DBUS_TYPE_INVALID);
}

void rb_dbus_append_int16(DBusMessage *msg, int16_t value) {
    dbus_message_append_args(msg, DBUS_TYPE_INT16, &value,
                             DBUS_TYPE_INVALID);
}

void rb_dbus_append_byte_array(DBusMessage *msg, const uint8_t *bytes,
                               size_t len) {
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &arr);
    if (len > 0 && bytes) {
        dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &bytes,
                                             (int)len);
    }
    dbus_message_iter_close_container(&it, &arr);
}

void rb_dbus_append_iter(DBusMessage *msg, DBusMessageIter *it) {
    dbus_message_iter_init_append(msg, it);
}

void rb_dbus_open_dict(DBusMessageIter *parent, DBusMessageIter *dict) {
    dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}",
                                     dict);
}

void rb_dbus_close_dict(DBusMessageIter *parent, DBusMessageIter *dict) {
    dbus_message_iter_close_container(parent, dict);
}

void rb_dbus_open_settings(DBusMessageIter *parent,
                           DBusMessageIter *outer) {
    dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                     outer);
}

void rb_dbus_close_settings(DBusMessageIter *parent,
                            DBusMessageIter *outer) {
    dbus_message_iter_close_container(parent, outer);
}

void rb_dbus_setting_open(DBusMessageIter *outer, rb_dbus_setting_t *s,
                          const char *name) {
    const char *n = name ? name : "";
    dbus_message_iter_open_container(outer, DBUS_TYPE_DICT_ENTRY, NULL,
                                     &s->entry);
    dbus_message_iter_append_basic(&s->entry, DBUS_TYPE_STRING, &n);
    /* Section value: plain a{sv} array (NOT a variant — see header). */
    dbus_message_iter_open_container(&s->entry, DBUS_TYPE_ARRAY, "{sv}",
                                     &s->dict);
}

void rb_dbus_setting_close(DBusMessageIter *outer, rb_dbus_setting_t *s) {
    dbus_message_iter_close_container(&s->entry, &s->dict);
    dbus_message_iter_close_container(outer, &s->entry);
}

static void dict_put_open(DBusMessageIter *dict, const char *key,
                          const char *sig, DBusMessageIter *entry,
                          DBusMessageIter *var) {
    const char *k = key ? key : "";
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL,
                                     entry);
    dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(entry, DBUS_TYPE_VARIANT, sig, var);
}

static void dict_put_close(DBusMessageIter *dict, DBusMessageIter *entry,
                           DBusMessageIter *var) {
    dbus_message_iter_close_container(entry, var);
    dbus_message_iter_close_container(dict, entry);
}

void rb_dbus_dict_put_string(DBusMessageIter *dict, const char *key,
                             const char *value) {
    DBusMessageIter entry, var;
    const char *v = value ? value : "";
    dict_put_open(dict, key, "s", &entry, &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
    dict_put_close(dict, &entry, &var);
}

void rb_dbus_dict_put_bool(DBusMessageIter *dict, const char *key,
                           int value) {
    DBusMessageIter entry, var;
    dbus_bool_t v = value ? TRUE : FALSE;
    dict_put_open(dict, key, "b", &entry, &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
    dict_put_close(dict, &entry, &var);
}

void rb_dbus_dict_put_uint32(DBusMessageIter *dict, const char *key,
                             uint32_t value) {
    DBusMessageIter entry, var;
    dict_put_open(dict, key, "u", &entry, &var);
    dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &value);
    dict_put_close(dict, &entry, &var);
}

void rb_dbus_dict_put_byte_array(DBusMessageIter *dict, const char *key,
                                 const uint8_t *bytes, size_t len) {
    DBusMessageIter entry, var, arr;
    dict_put_open(dict, key, "ay", &entry, &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "y", &arr);
    if (len > 0 && bytes) {
        dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &bytes,
                                             (int)len);
    }
    dbus_message_iter_close_container(&var, &arr);
    dict_put_close(dict, &entry, &var);
}

DBusMessage *rb_dbus_call(rb_dbus_conn_t *conn, DBusMessage *request,
                          uint32_t timeout_ms, char *err_name,
                          size_t err_name_len, char *detail,
                          size_t detail_len) {
    if (err_name && err_name_len) err_name[0] = '\0';
    if (detail && detail_len) detail[0] = '\0';
    if (!conn || !request) {
        if (err_name && err_name_len) copy_str(err_name, err_name_len,
                                                "disconnected");
        if (detail && detail_len)
            copy_str(detail, detail_len, "no bus connection");
        if (request) dbus_message_unref(request);
        return NULL;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn->conn, request,
        timeout_ms ? (int)timeout_ms : 5000, &err);
    dbus_message_unref(request);
    if (!reply) {
        const char *name =
            dbus_error_is_set(&err) ? err.name : "disconnected";
        const char *msg =
            dbus_error_is_set(&err) ? err.message : "no reply";
        if (err_name && err_name_len) copy_str(err_name, err_name_len,
                                                name);
        if (detail && detail_len) {
            if (msg && *msg)
                snprintf(detail, detail_len, "%s: %s", name, msg);
            else
                copy_str(detail, detail_len, name);
        }
        dbus_error_free(&err);
        return NULL;
    }
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *name = dbus_message_get_error_name(reply);
        if (err_name && err_name_len)
            copy_str(err_name, err_name_len, name ? name : "error");
        if (detail && detail_len) {
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                const char *m = NULL;
                dbus_message_iter_get_basic(&it, &m);
                snprintf(detail, detail_len, "%s: %s",
                         name ? name : "error", m ? m : "");
            } else {
                copy_str(detail, detail_len, name ? name : "error");
            }
        }
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

DBusMessage *rb_dbus_get_property(rb_dbus_conn_t *conn, const char *dest,
                                  const char *path, const char *iface,
                                  const char *prop, char *err_name,
                                  size_t err_name_len, char *detail,
                                  size_t detail_len) {
    DBusMessage *req = rb_dbus_new_call(dest, path,
                                        "org.freedesktop.DBus.Properties",
                                        "Get");
    if (!req) {
        if (err_name && err_name_len)
            copy_str(err_name, err_name_len, "disconnected");
        if (detail && detail_len)
            copy_str(detail, detail_len, "cannot allocate message");
        return NULL;
    }
    rb_dbus_append_string(req, iface);
    rb_dbus_append_string(req, prop);
    return rb_dbus_call(conn, req, 5000, err_name, err_name_len, detail,
                        detail_len);
}

int rb_dbus_name_has_owner(rb_dbus_conn_t *conn, const char *name,
                           char *detail, size_t detail_len) {
    char err_name[256];
    DBusMessage *req =
        rb_dbus_new_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                         "org.freedesktop.DBus", "NameHasOwner");
    if (!req) return -1;
    rb_dbus_append_string(req, name);
    DBusMessage *reply =
        rb_dbus_call(conn, req, 5000, err_name, sizeof(err_name), detail,
                     detail_len);
    if (!reply) return -1;
    DBusMessageIter it;
    int result = 0;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t v = FALSE;
        dbus_message_iter_get_basic(&it, &v);
        result = v ? 1 : 0;
    }
    dbus_message_unref(reply);
    return result;
}

/* --- Reply parsing -------------------------------------------------- */

static int variant_inner(DBusMessage *reply, DBusMessageIter *inner) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(reply, &it)) return 0;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT)
        return 0;
    dbus_message_iter_recurse(&it, inner);
    return 1;
}

int rb_dbus_variant_string(DBusMessage *reply, char *out, size_t out_len) {
    DBusMessageIter inner;
    if (!variant_inner(reply, &inner)) return 0;
    return rb_dbus_iter_string(&inner, out, out_len);
}

int rb_dbus_variant_int16(DBusMessage *reply, int16_t *out) {
    DBusMessageIter inner;
    if (!variant_inner(reply, &inner)) return 0;
    return rb_dbus_iter_int16(&inner, out);
}

int rb_dbus_variant_uint32(DBusMessage *reply, uint32_t *out) {
    DBusMessageIter inner;
    if (!variant_inner(reply, &inner)) return 0;
    return rb_dbus_iter_uint32(&inner, out);
}

int rb_dbus_variant_bool(DBusMessage *reply, int *out) {
    DBusMessageIter inner;
    if (!variant_inner(reply, &inner)) return 0;
    return rb_dbus_iter_bool(&inner, out);
}

int rb_dbus_variant_object_path(DBusMessage *reply, char *out,
                                size_t out_len) {
    DBusMessageIter inner;
    if (!variant_inner(reply, &inner)) return 0;
    return rb_dbus_iter_object_path(&inner, out, out_len);
}

int rb_dbus_variant_byte_array(DBusMessage *reply, uint8_t *out,
                               size_t cap, size_t *out_len) {
    DBusMessageIter inner;
    if (!variant_inner(reply, &inner)) return 0;
    return rb_dbus_iter_byte_array(&inner, out, cap, out_len);
}

void rb_dbus_reply_each_object_path(
    DBusMessage *reply, void (*cb)(const char *path, void *ud), void *ud) {
    DBusMessageIter it, arr;
    if (!cb) return;
    if (!dbus_message_iter_init(reply, &it)) return;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) ==
           DBUS_TYPE_OBJECT_PATH) {
        const char *p = NULL;
        dbus_message_iter_get_basic(&arr, &p);
        if (p) cb(p, ud);
        dbus_message_iter_next(&arr);
    }
}

int rb_dbus_dict_find(DBusMessageIter *dict, const char *key,
                      DBusMessageIter *val) {
    DBusMessageIter it = *dict;
    while (dbus_message_iter_get_arg_type(&it) ==
           DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&it, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            const char *k = NULL;
            dbus_message_iter_get_basic(&entry, &k);
            if (k && strcmp(k, key) == 0) {
                if (!dbus_message_iter_next(&entry)) return 0;
                if (dbus_message_iter_get_arg_type(&entry) !=
                    DBUS_TYPE_VARIANT)
                    return 0;
                dbus_message_iter_recurse(&entry, val);
                return 1;
            }
        }
        dbus_message_iter_next(&it);
    }
    return 0;
}

int rb_dbus_iter_string(DBusMessageIter *it, char *out, size_t out_len) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_STRING) return 0;
    const char *v = NULL;
    dbus_message_iter_get_basic(it, &v);
    copy_str(out, out_len, v);
    return 1;
}

int rb_dbus_iter_int16(DBusMessageIter *it, int16_t *out) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_INT16) return 0;
    dbus_message_iter_get_basic(it, out);
    return 1;
}

int rb_dbus_iter_uint32(DBusMessageIter *it, uint32_t *out) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_UINT32) return 0;
    dbus_message_iter_get_basic(it, out);
    return 1;
}

int rb_dbus_iter_bool(DBusMessageIter *it, int *out) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_BOOLEAN) return 0;
    dbus_bool_t v = FALSE;
    dbus_message_iter_get_basic(it, &v);
    *out = v ? 1 : 0;
    return 1;
}

int rb_dbus_iter_object_path(DBusMessageIter *it, char *out,
                             size_t out_len) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_OBJECT_PATH)
        return 0;
    const char *v = NULL;
    dbus_message_iter_get_basic(it, &v);
    copy_str(out, out_len, v);
    return 1;
}

int rb_dbus_iter_byte_array(DBusMessageIter *it, uint8_t *out, size_t cap,
                            size_t *out_len) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) return 0;
    if (dbus_message_iter_get_element_type(it) != DBUS_TYPE_BYTE)
        return 0;
    DBusMessageIter arr;
    dbus_message_iter_recurse(it, &arr);
    void *raw = NULL;
    int n = 0;
    dbus_message_iter_get_fixed_array(&arr, &raw, &n);
    const uint8_t *data = (const uint8_t *)raw;
    if (n < 0) n = 0;
    size_t copy = (size_t)n < cap ? (size_t)n : cap;
    if (copy > 0 && data) memcpy(out, data, copy);
    if (out_len) *out_len = copy;
    return 1;
}

void rb_dbus_iter_each_string(DBusMessageIter *it,
                              void (*cb)(const char *s, void *ud),
                              void *ud) {
    DBusMessageIter arr = *it;
    if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_ARRAY) return;
    if (dbus_message_iter_get_element_type(&arr) != DBUS_TYPE_STRING)
        return;
    dbus_message_iter_recurse(&arr, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *s = NULL;
        dbus_message_iter_get_basic(&arr, &s);
        if (s && cb) cb(s, ud);
        dbus_message_iter_next(&arr);
    }
}

void rb_dbus_iter_each_manufacturer(
    DBusMessageIter *it,
    void (*cb)(uint16_t id, const uint8_t *data, size_t len, void *ud),
    void *ud) {
    DBusMessageIter dict = *it;
    if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&dict, &dict);
    while (dbus_message_iter_get_arg_type(&dict) ==
           DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, val, arr;
        dbus_message_iter_recurse(&dict, &entry);
        uint16_t id = 0;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_UINT16)
            dbus_message_iter_get_basic(&entry, &id);
        if (!dbus_message_iter_next(&entry)) {
            dbus_message_iter_next(&dict);
            continue;
        }
        /* BlueZ spec: a{qay} values are PLAIN byte arrays. Accept a
         * variant-wrapped array too (tolerance for non-conformant
         * peers), but never require it. */
        val = entry;
        if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_VARIANT)
            dbus_message_iter_recurse(&val, &val);
        if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_ARRAY &&
            dbus_message_iter_get_element_type(&val) == DBUS_TYPE_BYTE) {
            dbus_message_iter_recurse(&val, &arr);
            void *raw = NULL;
            int n = 0;
            dbus_message_iter_get_fixed_array(&arr, &raw, &n);
            const uint8_t *data = (const uint8_t *)raw;
            if (n > 0 && data && cb) cb(id, data, (size_t)n, ud);
        }
        dbus_message_iter_next(&dict);
    }
}

void rb_dbus_walk_managed_objects(
    DBusMessage *reply,
    void (*cb)(const char *path, const char *iface, DBusMessageIter *props,
               void *ud),
    void *ud) {
    DBusMessageIter it, objects;
    if (!cb) return;
    if (!dbus_message_iter_init(reply, &it)) return;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&it, &objects);
    while (dbus_message_iter_get_arg_type(&objects) ==
           DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter obj_entry, ifaces;
        dbus_message_iter_recurse(&objects, &obj_entry);
        const char *path = NULL;
        if (dbus_message_iter_get_arg_type(&obj_entry) ==
            DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&obj_entry, &path);
        if (dbus_message_iter_next(&obj_entry) &&
            dbus_message_iter_get_arg_type(&obj_entry) ==
                DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&obj_entry, &ifaces);
            while (dbus_message_iter_get_arg_type(&ifaces) ==
                   DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter iface_entry, props;
                dbus_message_iter_recurse(&ifaces, &iface_entry);
                const char *iface = NULL;
                if (dbus_message_iter_get_arg_type(&iface_entry) ==
                    DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&iface_entry, &iface);
                if (dbus_message_iter_next(&iface_entry) &&
                    dbus_message_iter_get_arg_type(&iface_entry) ==
                        DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&iface_entry, &props);
                    if (path && iface) cb(path, iface, &props, ud);
                }
                dbus_message_iter_next(&ifaces);
            }
        }
        dbus_message_iter_next(&objects);
    }
}
