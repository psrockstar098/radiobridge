/*
 * RadioBridge — rb_linux.c (Linux backend, Phase 2)
 *
 * Real BlueZ + NetworkManager support over D-Bus (libdbus). The
 * backend speaks the documented interfaces:
 *
 *   BlueZ: org.bluez Adapter1 (StartDiscovery/StopDiscovery),
 *     Device1 (Connect/Disconnect, Address/Name/Alias/RSSI/
 *     ManufacturerData/UUIDs/Connected/Paired/ServicesResolved),
 *     GattService1/GattCharacteristic1/GattDescriptor1
 *     (ReadValue/WriteValue/StartNotify/StopNotify).
 *   NetworkManager: root GetDevices/AddAndActivateConnection/
 *     DeactivateConnection/Version, Device.Wireless RequestScan/
 *     GetAccessPoints/ActiveAccessPoint, AccessPoint properties,
 *     Device State, Ip4Config AddressData/Gateway.
 *
 * Bus address resolution (see rb_dbus.h): RB_DBUS_ADDRESS, then
 * DBUS_SYSTEM_BUS_ADDRESS, then the system bus. That is what makes
 * the fake-bus test possible: the backend under test connects to a
 * private bus owned by a fake service, over the real bus protocol.
 *
 * DOCUMENTED v0.2 SIMPLIFICATIONS (not hidden limitations):
 *  - BLE scan is snapshot-based: StartDiscovery, a short settle, then
 *    GetManagedObjects snapshots repeated until stopped or the
 *    timeout elapses. Advertisement signals are not consumed; a
 *    device that stops advertising mid-scan may still appear in the
 *    last snapshot. Delivery dedup follows allow_duplicates.
 *  - BlueZ exposes no numeric GATT handles on D-Bus, so discovery
 *    assigns synthetic sequential handles (services 1.., chars 1..,
 *    descriptors 1..) in snapshot order.
 *  - BlueZ does not expose connectability, so discovered devices are
 *    reported connectable=1.
 *  - NetworkManager reports signal as Strength percent (0-100), not
 *    dBm, so Wi-Fi scan/status report signal_dbm=0 ("not reported")
 *    rather than a fabricated percent-to-dBm mapping.
 *  - AP security mapping is conservative: RsnFlags present -> WPA2
 *    (WPA3/SAE is not distinguished from WPA2 via flags in v0.2);
 *    WpaFlags present -> WPA; privacy bit only -> WEP; all zero ->
 *    OPEN.
 *  - Notifications are poll-based: after StartNotify, a thread polls
 *    the characteristic Value property every 150 ms and delivers on
 *    change. This avoids libdbus dispatch-thread hazards (all bus use
 *    is blocking calls) at the cost of ~150 ms delivery latency.
 *    Signals are not consumed in v0.2.
 *  - Wi-Fi connect uses key-mgmt "wpa-psk" for secured personal
 *    networks (per NM docs this covers WPA2 + WPA3 transition);
 *    802.1X enterprise is NOT_SUPPORTED in v0.2.
 *  - Reconnect policy is stored but auto-reconnect is not attempted
 *    on Linux in v0.2 (no signal delivery to detect drops); the
 *    reconnect capability therefore reports UNAVAILABLE.
 *  - BLE connect is asynchronous (worker thread; CONNECTING is
 *    emitted synchronously, CONNECTED/FAILED later), matching the
 *    public contract; unknown addresses report FAILED/NOT_FOUND
 *    asynchronously.
 *  - Wi-Fi connect is asynchronous (activation requested, outcome
 *    via rb_wifi_get_status()); v0.2 emits no RB_EVENT_WIFI_CHANGED
 *    events (no signal delivery), so status polling is the way to
 *    observe the result. Disconnect returns OK when already
 *    disconnected, per the public contract.
 *  - Quality is a real POSIX TCP-connect latency probe (3 samples),
 *    not ICMP and not radio metrics.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rb_internal.h"
#include "radiobridge/rb_diagnostics.h"

#if defined(RB_HAVE_DBUS) && !defined(_WIN32)
#include "rb_dbus.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Interface definitions (grounded in BlueZ + NetworkManager docs)     */
/* ------------------------------------------------------------------ */

#if defined(RB_HAVE_DBUS) && !defined(_WIN32)
static const char *RB_BLUEZ_SERVICE = "org.bluez";
static const char *RB_DBUS_OBJECT_MANAGER =
    "org.freedesktop.DBus.ObjectManager";
static const char *RB_BLUEZ_ADAPTER_IFACE = "org.bluez.Adapter1";
static const char *RB_BLUEZ_DEVICE_IFACE = "org.bluez.Device1";
static const char *RB_BLUEZ_GATT_SERVICE_IFACE = "org.bluez.GattService1";
static const char *RB_BLUEZ_GATT_CHAR_IFACE =
    "org.bluez.GattCharacteristic1";
static const char *RB_BLUEZ_GATT_DESC_IFACE = "org.bluez.GattDescriptor1";

static const char *RB_NM_SERVICE = "org.freedesktop.NetworkManager";
static const char *RB_NM_ROOT_PATH = "/org/freedesktop/NetworkManager";
static const char *RB_NM_IFACE = "org.freedesktop.NetworkManager";
static const char *RB_NM_DEVICE_IFACE = "org.freedesktop.NetworkManager.Device";
static const char *RB_NM_WIRELESS_IFACE =
    "org.freedesktop.NetworkManager.Device.Wireless";
static const char *RB_NM_AP_IFACE =
    "org.freedesktop.NetworkManager.AccessPoint";
static const char *RB_NM_IP4_IFACE =
    "org.freedesktop.NetworkManager.IP4Config";

/* NM device types / states (NetworkManager D-Bus API). */
#define RB_NM_DEVICE_TYPE_WIFI 2
#define RB_NM_STATE_DISCONNECTED 30
#define RB_NM_STATE_PREPARE 40
#define RB_NM_STATE_CONFIG 50
#define RB_NM_STATE_NEED_AUTH 60
#define RB_NM_STATE_IP_CONFIG 70
#define RB_NM_STATE_IP_CHECK 80
#define RB_NM_STATE_SECONDARIES 90
#define RB_NM_STATE_ACTIVATED 100
#define RB_NM_STATE_DEACTIVATING 110
#define RB_NM_STATE_FAILED 120

#define RB_LINUX_BT_SYSFS "/sys/class/bluetooth"
#define RB_LINUX_DBUS_SOCKET "/run/dbus/system_bus_socket"
#define RB_LINUX_NM_RUNDIR "/run/NetworkManager"

#define RB_LINUX_MAX_DEVICES 32
#define RB_LINUX_MAX_SERVICES 32
#define RB_LINUX_MAX_CHARS 128
#define RB_LINUX_MAX_DESCS 128
#define RB_LINUX_MAX_APS 32
#define RB_LINUX_MAX_SUBS 8

#endif /* DBUS interface constants */

#if !defined(RB_HAVE_DBUS) || defined(_WIN32)
/* Without libdbus there is no transport: keep the Phase 1 honest
 * UNAVAILABLE behavior instead of failing the build. */
rb_status_t rb_linux_probe(rb_capabilities_t *caps) {
    if (caps == NULL) return RB_ERROR_INVALID_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
    rb_copy_str(caps->backend_name, sizeof(caps->backend_name), "linux");
    rb_copy_str(caps->backend_version, sizeof(caps->backend_version),
                "0.2.0");
    rb_copy_str(caps->os_name, sizeof(caps->os_name), "linux");
    caps->ble.scan = RB_CAP_UNAVAILABLE;
    caps->ble.connect = RB_CAP_UNAVAILABLE;
    caps->ble.gatt = RB_CAP_UNAVAILABLE;
    caps->ble.notify = RB_CAP_UNAVAILABLE;
    caps->ble.pairing = RB_CAP_UNAVAILABLE;
    caps->ble.rssi = RB_CAP_UNAVAILABLE;
    caps->ble.reconnect = RB_CAP_UNAVAILABLE;
    caps->wifi.status = RB_CAP_UNAVAILABLE;
    caps->wifi.scan = RB_CAP_UNAVAILABLE;
    caps->wifi.connect = RB_CAP_UNAVAILABLE;
    caps->wifi.quality = RB_CAP_UNAVAILABLE;
    caps->wifi.saved_networks = RB_CAP_UNAVAILABLE;
    rb_copy_str(caps->detail, sizeof(caps->detail),
                "linux backend: libdbus not available at build time; "
                "D-Bus transport disabled, every operation unavailable; "
                "services absent; evidence paths: "
                "/sys/class/bluetooth, "
                "/run/dbus/system_bus_socket, /run/NetworkManager");
    return RB_OK;
}
#endif /* no DBUS or Windows */

#if defined(RB_HAVE_DBUS) && !defined(_WIN32)

/* ------------------------------------------------------------------ */
/* Backend state                                                       */
/* ------------------------------------------------------------------ */

typedef struct linux_sub {
    int active;
    char device_address[32];
    char char_path[256];
    rb_uuid_t svc_uuid;
    rb_uuid_t chr_uuid;
    rb_ble_notify_callback_t cb;
    void *ud;
    rb_thread_t thread;
    int stop;
    uint8_t last[RB_BLE_MAX_VALUE_LEN];
    size_t last_len;
    int have_last;
} linux_sub_t;

typedef struct linux_state {
    rb_context_t *ctx;
    rb_dbus_conn_t *bus;
    int bluez_present;
    int nm_present;
    char adapter_path[256];
    char adapter_address[64];
    int adapter_powered; /* 1/0/-1 unknown */
    char nm_version[64];
    char wifi_device_path[256];
    char active_conn_path[256];
    char connected_ssid[64];
    rb_reconnect_policy_t reconnect_policy;
    rb_mutex_t lock;
    /* BLE scan job. */
    int scan_active;
    rb_thread_t scan_thread;
    int scan_stop;
    rb_operation_id_t scan_op;
    /* Wi-Fi scan job. */
    int wifi_scan_active;
    rb_thread_t wifi_scan_thread;
    int wifi_scan_stop;
    rb_operation_id_t wifi_scan_op;
    linux_sub_t subs[RB_LINUX_MAX_SUBS];
} linux_state_t;

static linux_state_t *linux_get(rb_context_t *ctx) {
    return (linux_state_t *)ctx->backend_state;
}

/* ------------------------------------------------------------------ */
/* Snapshot collectors                                                 */
/* ------------------------------------------------------------------ */

typedef struct bt_device_snap {
    char path[256];
    char address[32];
    char name[128];
    char alias[128];
    int has_rssi;
    int16_t rssi;
    int has_tx_power;
    int16_t tx_power;
    rb_uuid_t uuids[RB_BLE_MAX_SERVICE_UUIDS];
    size_t uuid_count;
    int has_mfr;
    uint16_t mfr_id;
    uint8_t mfr_data[RB_BLE_MAX_MANUFACTURER_DATA];
    size_t mfr_len;
    int connected;
    int paired;
    int services_resolved;
} bt_device_snap_t;

typedef struct bt_service_snap {
    char path[256];
    char device_path[256];
    rb_uuid_t uuid;
} bt_service_snap_t;

typedef struct bt_char_snap {
    char path[256];
    char service_path[256];
    rb_uuid_t uuid;
    char flags[8][32];
    size_t flag_count;
} bt_char_snap_t;

typedef struct bt_desc_snap {
    char path[256];
    char char_path[256];
    rb_uuid_t uuid;
} bt_desc_snap_t;

typedef struct bt_snapshot {
    bt_device_snap_t devices[RB_LINUX_MAX_DEVICES];
    size_t device_count;
    bt_service_snap_t services[RB_LINUX_MAX_SERVICES];
    size_t service_count;
    bt_char_snap_t chars[RB_LINUX_MAX_CHARS];
    size_t char_count;
    bt_desc_snap_t descs[RB_LINUX_MAX_DESCS];
    size_t desc_count;
} bt_snapshot_t;

typedef struct nm_device_snap {
    char path[256];
    uint32_t type;
    uint32_t state;
    char active_ap[256];
    char ip4config[256];
    char active_connection[256];
} nm_device_snap_t;

typedef struct nm_ap_snap {
    char path[256];
    uint8_t ssid[64];
    size_t ssid_len;
    char hw_address[32];
    uint32_t strength;
    uint32_t flags;
    uint32_t wpa_flags;
    uint32_t rsn_flags;
    uint32_t frequency;
    uint32_t mode;
} nm_ap_snap_t;

typedef struct nm_ip4_snap {
    char path[256];
    char gateway[64];
    char address[64];
} nm_ip4_snap_t;

typedef struct nm_snapshot {
    nm_device_snap_t devices[8];
    size_t device_count;
    nm_ap_snap_t aps[RB_LINUX_MAX_APS];
    size_t ap_count;
    nm_ip4_snap_t ip4s[8];
    size_t ip4_count;
    char version[64];
} nm_snapshot_t;

static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* Case-insensitive substring search (no strcasestr dependency). */
static int contains_ci(const char *haystack, const char *needle) {
    if (!*needle) return 1;
    for (const char *h = haystack; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++;
            b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

typedef struct uuid_collect {
    rb_uuid_t *out;
    size_t *count;
    size_t max;
} uuid_collect_t;

static void uuid_collect_cb(const char *s, void *ud) {
    uuid_collect_t *c = (uuid_collect_t *)ud;
    if (*c->count >= c->max) return;
    rb_uuid_t u;
    if (rb_uuid_from_string(s, &u) == RB_OK) c->out[(*c->count)++] = u;
}

typedef struct mfr_collect {
    bt_device_snap_t *dev;
} mfr_collect_t;

static void mfr_collect_cb(uint16_t id, const uint8_t *data, size_t len,
                           void *ud) {
    mfr_collect_t *c = (mfr_collect_t *)ud;
    if (c->dev->has_mfr) return; /* first entry only; struct holds one */
    c->dev->has_mfr = 1;
    c->dev->mfr_id = id;
    c->dev->mfr_len = len < sizeof(c->dev->mfr_data)
                          ? len
                          : sizeof(c->dev->mfr_data);
    memcpy(c->dev->mfr_data, data, c->dev->mfr_len);
}

typedef struct flag_collect {
    bt_char_snap_t *chr;
} flag_collect_t;

static void flag_collect_cb(const char *s, void *ud) {
    flag_collect_t *c = (flag_collect_t *)ud;
    if (c->chr->flag_count >= 8) return;
    rb_copy_str(c->chr->flags[c->chr->flag_count++], 32, s);
}

static void bt_mo_cb(const char *path, const char *iface,
                     DBusMessageIter *props, void *ud) {
    bt_snapshot_t *snap = (bt_snapshot_t *)ud;
    DBusMessageIter v;
    if (strcmp(iface, RB_BLUEZ_DEVICE_IFACE) == 0) {
        if (snap->device_count >= RB_LINUX_MAX_DEVICES) return;
        bt_device_snap_t *d = &snap->devices[snap->device_count++];
        memset(d, 0, sizeof(*d));
        rb_copy_str(d->path, sizeof(d->path), path);
        if (rb_dbus_dict_find(props, "Address", &v))
            rb_dbus_iter_string(&v, d->address, sizeof(d->address));
        if (rb_dbus_dict_find(props, "Name", &v))
            rb_dbus_iter_string(&v, d->name, sizeof(d->name));
        if (rb_dbus_dict_find(props, "Alias", &v))
            rb_dbus_iter_string(&v, d->alias, sizeof(d->alias));
        if (rb_dbus_dict_find(props, "RSSI", &v)) {
            int16_t r = 0;
            if (rb_dbus_iter_int16(&v, &r)) {
                d->has_rssi = 1;
                d->rssi = r;
            }
        }
        if (rb_dbus_dict_find(props, "TxPower", &v)) {
            int16_t t = 0;
            if (rb_dbus_iter_int16(&v, &t)) {
                d->has_tx_power = 1;
                d->tx_power = t;
            }
        }
        if (rb_dbus_dict_find(props, "UUIDs", &v)) {
            uuid_collect_t c = {d->uuids, &d->uuid_count,
                                RB_BLE_MAX_SERVICE_UUIDS};
            rb_dbus_iter_each_string(&v, uuid_collect_cb, &c);
        }
        if (rb_dbus_dict_find(props, "ManufacturerData", &v)) {
            mfr_collect_t c = {d};
            rb_dbus_iter_each_manufacturer(&v, mfr_collect_cb, &c);
        }
        if (rb_dbus_dict_find(props, "Connected", &v))
            rb_dbus_iter_bool(&v, &d->connected);
        if (rb_dbus_dict_find(props, "Paired", &v))
            rb_dbus_iter_bool(&v, &d->paired);
        if (rb_dbus_dict_find(props, "ServicesResolved", &v))
            rb_dbus_iter_bool(&v, &d->services_resolved);
        return;
    }
    if (strcmp(iface, RB_BLUEZ_GATT_SERVICE_IFACE) == 0) {
        if (snap->service_count >= RB_LINUX_MAX_SERVICES) return;
        bt_service_snap_t *s = &snap->services[snap->service_count++];
        memset(s, 0, sizeof(*s));
        rb_copy_str(s->path, sizeof(s->path), path);
        /* Device path = service path minus the last component. */
        rb_copy_str(s->device_path, sizeof(s->device_path), path);
        char *slash = strrchr(s->device_path, '/');
        if (slash) *slash = '\0';
        if (rb_dbus_dict_find(props, "UUID", &v)) {
            char buf[64];
            if (rb_dbus_iter_string(&v, buf, sizeof(buf)))
                rb_uuid_from_string(buf, &s->uuid);
        }
        return;
    }
    if (strcmp(iface, RB_BLUEZ_GATT_CHAR_IFACE) == 0) {
        if (snap->char_count >= RB_LINUX_MAX_CHARS) return;
        bt_char_snap_t *c = &snap->chars[snap->char_count++];
        memset(c, 0, sizeof(*c));
        rb_copy_str(c->path, sizeof(c->path), path);
        rb_copy_str(c->service_path, sizeof(c->service_path), path);
        char *slash = strrchr(c->service_path, '/');
        if (slash) *slash = '\0';
        if (rb_dbus_dict_find(props, "UUID", &v)) {
            char buf[64];
            if (rb_dbus_iter_string(&v, buf, sizeof(buf)))
                rb_uuid_from_string(buf, &c->uuid);
        }
        if (rb_dbus_dict_find(props, "Flags", &v)) {
            flag_collect_t fc = {c};
            rb_dbus_iter_each_string(&v, flag_collect_cb, &fc);
        }
        return;
    }
    if (strcmp(iface, RB_BLUEZ_GATT_DESC_IFACE) == 0) {
        if (snap->desc_count >= RB_LINUX_MAX_DESCS) return;
        bt_desc_snap_t *d = &snap->descs[snap->desc_count++];
        memset(d, 0, sizeof(*d));
        rb_copy_str(d->path, sizeof(d->path), path);
        rb_copy_str(d->char_path, sizeof(d->char_path), path);
        char *slash = strrchr(d->char_path, '/');
        if (slash) *slash = '\0';
        if (rb_dbus_dict_find(props, "UUID", &v)) {
            char buf[64];
            if (rb_dbus_iter_string(&v, buf, sizeof(buf)))
                rb_uuid_from_string(buf, &d->uuid);
        }
        return;
    }
}

static int bt_snapshot_take(rb_dbus_conn_t *bus, bt_snapshot_t *snap,
                            char *err_name, size_t enl, char *detail,
                            size_t dl) {
    memset(snap, 0, sizeof(*snap));
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, "/",
                                        RB_DBUS_OBJECT_MANAGER,
                                        "GetManagedObjects");
    if (!req) return 0;
    DBusMessage *reply =
        rb_dbus_call(bus, req, 5000, err_name, enl, detail, dl);
    if (!reply) return 0;
    rb_dbus_walk_managed_objects(reply, bt_mo_cb, snap);
    dbus_message_unref(reply);
    return 1;
}

typedef struct addr_data_collect {
    nm_ip4_snap_t *ip;
} addr_data_collect_t;

/* AddressData is aa{sv}: each entry is a dict; we take "address" from
 * the first entry. Handled by a dedicated walker below. */
static void nm_mo_cb(const char *path, const char *iface,
                     DBusMessageIter *props, void *ud) {
    nm_snapshot_t *snap = (nm_snapshot_t *)ud;
    DBusMessageIter v;
    if (strcmp(iface, RB_NM_IFACE) == 0 &&
        strcmp(path, RB_NM_ROOT_PATH) == 0) {
        if (rb_dbus_dict_find(props, "Version", &v))
            rb_dbus_iter_string(&v, snap->version, sizeof(snap->version));
        return;
    }
    if (strcmp(iface, RB_NM_DEVICE_IFACE) == 0) {
        if (snap->device_count >= 8) return;
        nm_device_snap_t *d = &snap->devices[snap->device_count++];
        memset(d, 0, sizeof(*d));
        rb_copy_str(d->path, sizeof(d->path), path);
        if (rb_dbus_dict_find(props, "DeviceType", &v))
            rb_dbus_iter_uint32(&v, &d->type);
        if (rb_dbus_dict_find(props, "State", &v))
            rb_dbus_iter_uint32(&v, &d->state);
        if (rb_dbus_dict_find(props, "ActiveAccessPoint", &v))
            rb_dbus_iter_object_path(&v, d->active_ap,
                                     sizeof(d->active_ap));
        if (rb_dbus_dict_find(props, "Ip4Config", &v))
            rb_dbus_iter_object_path(&v, d->ip4config,
                                     sizeof(d->ip4config));
        if (rb_dbus_dict_find(props, "ActiveConnection", &v))
            rb_dbus_iter_object_path(&v, d->active_connection,
                                     sizeof(d->active_connection));
        return;
    }
    if (strcmp(iface, RB_NM_AP_IFACE) == 0) {
        if (snap->ap_count >= RB_LINUX_MAX_APS) return;
        nm_ap_snap_t *a = &snap->aps[snap->ap_count++];
        memset(a, 0, sizeof(*a));
        rb_copy_str(a->path, sizeof(a->path), path);
        if (rb_dbus_dict_find(props, "Ssid", &v))
            rb_dbus_iter_byte_array(&v, a->ssid, sizeof(a->ssid),
                                    &a->ssid_len);
        if (rb_dbus_dict_find(props, "HwAddress", &v))
            rb_dbus_iter_string(&v, a->hw_address, sizeof(a->hw_address));
        if (rb_dbus_dict_find(props, "Strength", &v))
            rb_dbus_iter_uint32(&v, &a->strength);
        if (rb_dbus_dict_find(props, "Flags", &v))
            rb_dbus_iter_uint32(&v, &a->flags);
        if (rb_dbus_dict_find(props, "WpaFlags", &v))
            rb_dbus_iter_uint32(&v, &a->wpa_flags);
        if (rb_dbus_dict_find(props, "RsnFlags", &v))
            rb_dbus_iter_uint32(&v, &a->rsn_flags);
        if (rb_dbus_dict_find(props, "Frequency", &v))
            rb_dbus_iter_uint32(&v, &a->frequency);
        if (rb_dbus_dict_find(props, "Mode", &v))
            rb_dbus_iter_uint32(&v, &a->mode);
        return;
    }
    if (strcmp(iface, RB_NM_IP4_IFACE) == 0) {
        if (snap->ip4_count >= 8) return;
        nm_ip4_snap_t *ip = &snap->ip4s[snap->ip4_count++];
        memset(ip, 0, sizeof(*ip));
        rb_copy_str(ip->path, sizeof(ip->path), path);
        if (rb_dbus_dict_find(props, "Gateway", &v))
            rb_dbus_iter_string(&v, ip->gateway, sizeof(ip->gateway));
        /* AddressData: aa{sv} — array of address dicts; take
         * "address" from the first dict. */
        if (rb_dbus_dict_find(props, "AddressData", &v)) {
            DBusMessageIter outer = v;
            if (dbus_message_iter_get_arg_type(&outer) ==
                DBUS_TYPE_ARRAY) {
                DBusMessageIter addrs;
                dbus_message_iter_recurse(&outer, &addrs);
                while (dbus_message_iter_get_arg_type(&addrs) ==
                       DBUS_TYPE_ARRAY) {
                    DBusMessageIter dict;
                    dbus_message_iter_recurse(&addrs, &dict);
                    DBusMessageIter av;
                    if (rb_dbus_dict_find(&dict, "address", &av)) {
                        rb_dbus_iter_string(&av, ip->address,
                                            sizeof(ip->address));
                        break;
                    }
                    dbus_message_iter_next(&addrs);
                }
            }
        }
        return;
    }
}

static int nm_snapshot_take(rb_dbus_conn_t *bus, nm_snapshot_t *snap,
                            char *err_name, size_t enl, char *detail,
                            size_t dl) {
    memset(snap, 0, sizeof(*snap));
    DBusMessage *req = rb_dbus_new_call(RB_NM_SERVICE, RB_NM_ROOT_PATH,
                                        RB_DBUS_OBJECT_MANAGER,
                                        "GetManagedObjects");
    if (!req) return 0;
    DBusMessage *reply =
        rb_dbus_call(bus, req, 5000, err_name, enl, detail, dl);
    if (!reply) return 0;
    rb_dbus_walk_managed_objects(reply, nm_mo_cb, snap);
    dbus_message_unref(reply);
    if (snap->version[0] == '\0') {
        /* Fallback: Version property directly on the root object. */
        DBusMessage *vr = rb_dbus_get_property(
            bus, RB_NM_SERVICE, RB_NM_ROOT_PATH, RB_NM_IFACE, "Version",
            err_name, enl, detail, dl);
        if (vr) {
            rb_dbus_variant_string(vr, snap->version,
                                   sizeof(snap->version));
            dbus_message_unref(vr);
        }
    }
    return 1;
}

static const bt_device_snap_t *bt_find_device(const bt_snapshot_t *snap,
                                              const char *address) {
    for (size_t i = 0; i < snap->device_count; i++)
        if (strcmp(snap->devices[i].address, address) == 0)
            return &snap->devices[i];
    return NULL;
}

static const nm_device_snap_t *nm_find_wifi(const nm_snapshot_t *snap) {
    for (size_t i = 0; i < snap->device_count; i++)
        if (snap->devices[i].type == RB_NM_DEVICE_TYPE_WIFI)
            return &snap->devices[i];
    return NULL;
}

static const nm_ap_snap_t *nm_find_ap(const nm_snapshot_t *snap,
                                      const char *path) {
    for (size_t i = 0; i < snap->ap_count; i++)
        if (strcmp(snap->aps[i].path, path) == 0) return &snap->aps[i];
    return NULL;
}

static const nm_ip4_snap_t *nm_find_ip4(const nm_snapshot_t *snap,
                                        const char *path) {
    for (size_t i = 0; i < snap->ip4_count; i++)
        if (strcmp(snap->ip4s[i].path, path) == 0) return &snap->ip4s[i];
    return NULL;
}

static rb_wifi_security_t nm_ap_security(const nm_ap_snap_t *a) {
    if (a->rsn_flags != 0) return RB_WIFI_SEC_WPA2;
    if (a->wpa_flags != 0) return RB_WIFI_SEC_WPA;
    if (a->flags & 1u) return RB_WIFI_SEC_WEP; /* privacy bit */
    if (a->flags == 0 && a->wpa_flags == 0 && a->rsn_flags == 0)
        return RB_WIFI_SEC_OPEN;
    return RB_WIFI_SEC_UNKNOWN;
}

static uint32_t nm_channel_for_freq(uint32_t freq) {
    if (freq == 2484) return 14;
    if (freq >= 2412 && freq <= 2472) return (freq - 2407) / 5;
    if (freq >= 5170 && freq <= 5825) return (freq - 5000) / 5;
    if (freq >= 5955 && freq <= 7115) return (freq - 5950) / 5;
    return 0;
}

static rb_wifi_state_t nm_state_to_wifi(uint32_t st, rb_wifi_diag_t *diag) {
    switch (st) {
        case RB_NM_STATE_ACTIVATED:
            *diag = RB_WIFI_DIAG_OK;
            return RB_WIFI_CONNECTED;
        case RB_NM_STATE_PREPARE:
        case RB_NM_STATE_CONFIG:
        case RB_NM_STATE_NEED_AUTH:
        case RB_NM_STATE_IP_CONFIG:
        case RB_NM_STATE_IP_CHECK:
        case RB_NM_STATE_SECONDARIES:
            *diag = RB_WIFI_DIAG_OK;
            return RB_WIFI_CONNECTING;
        case RB_NM_STATE_DISCONNECTED:
        case RB_NM_STATE_DEACTIVATING:
            *diag = RB_WIFI_DIAG_DISCONNECTED;
            return RB_WIFI_DISCONNECTED;
        case RB_NM_STATE_FAILED:
            *diag = RB_WIFI_DIAG_FAILED;
            return RB_WIFI_FAILED;
        default:
            *diag = RB_WIFI_DIAG_UNAVAILABLE;
            return RB_WIFI_UNAVAILABLE;
    }
}

/* ------------------------------------------------------------------ */
/* Probe / create / destroy / diagnostics                              */
/* ------------------------------------------------------------------ */

static void linux_fill_caps(rb_capabilities_t *caps, int bluez,
                            int nm, const char *detail) {
    memset(caps, 0, sizeof(*caps));
    rb_copy_str(caps->backend_name, sizeof(caps->backend_name), "linux");
    rb_copy_str(caps->backend_version, sizeof(caps->backend_version),
                "0.2.0");
    rb_copy_str(caps->os_name, sizeof(caps->os_name), "linux");
    rb_cap_state_t bt = bluez ? RB_CAP_AVAILABLE : RB_CAP_UNAVAILABLE;
    rb_cap_state_t wf = nm ? RB_CAP_AVAILABLE : RB_CAP_UNAVAILABLE;
    caps->ble.scan = bt;
    caps->ble.connect = bt;
    caps->ble.gatt = bt;
    caps->ble.notify = bt;
    caps->ble.pairing = bt;
    caps->ble.rssi = bt;
    /* Reconnect policy is stored but auto-reconnect is not attempted
     * on Linux in v0.2 (no signal delivery to detect drops). */
    caps->ble.reconnect = RB_CAP_UNAVAILABLE;
    caps->wifi.status = wf;
    caps->wifi.scan = wf;
    caps->wifi.connect = wf;
    /* Quality is a local TCP probe; it works without NetworkManager. */
    caps->wifi.quality = RB_CAP_AVAILABLE;
    caps->wifi.saved_networks = RB_CAP_NOT_SUPPORTED;
    rb_copy_str(caps->detail, sizeof(caps->detail), detail);
}

rb_status_t rb_linux_probe(rb_capabilities_t *caps) {
    if (caps == NULL) return RB_ERROR_INVALID_ARGUMENT;
    char detail[512];
    char bus_detail[256] = "";
    rb_dbus_conn_t *bus = NULL;
    rb_status_t st = rb_dbus_open(&bus, bus_detail, sizeof(bus_detail));
    int bluez = 0, nm = 0;
    int has_adapter = rb_path_exists(RB_LINUX_BT_SYSFS);
    int has_bus = rb_path_exists(RB_LINUX_DBUS_SOCKET);
    int has_nm = rb_path_exists(RB_LINUX_NM_RUNDIR);
    char bus_addr[512] = "unreachable";
    if (st == RB_OK && bus) {
        rb_copy_str(bus_addr, sizeof(bus_addr), rb_dbus_address(bus));
        const char *env = getenv("RB_DBUS_ADDRESS");
        if (!env || !*env) env = getenv("DBUS_SYSTEM_BUS_ADDRESS");
        if (env && *env) rb_copy_str(bus_addr, sizeof(bus_addr), env);
        char d2[256] = "";
        int r = rb_dbus_name_has_owner(bus, RB_BLUEZ_SERVICE, d2,
                                       sizeof(d2));
        bluez = r == 1;
        r = rb_dbus_name_has_owner(bus, RB_NM_SERVICE, d2, sizeof(d2));
        nm = r == 1;
        rb_dbus_close(bus);
    }
    snprintf(detail, sizeof(detail),
             "linux backend: BlueZ service %s, NetworkManager service "
             "%s on bus %s; adapter sysfs %s (%s), D-Bus socket %s "
             "(%s), NM rundir %s (%s)",
             bluez ? "present" : "absent", nm ? "present" : "absent",
             bus_addr, has_adapter ? "present" : "absent",
             RB_LINUX_BT_SYSFS, has_bus ? "present" : "absent",
             RB_LINUX_DBUS_SOCKET, has_nm ? "present" : "absent",
             RB_LINUX_NM_RUNDIR);
    linux_fill_caps(caps, bluez, nm, detail);
    return RB_OK;
}

/* Adapter discovery callback for the create() MO walk. */
static void linux_find_adapter_cb(const char *path, const char *iface,
                                  DBusMessageIter *props, void *ud) {
    linux_state_t *s = (linux_state_t *)ud;
    if (strcmp(iface, RB_BLUEZ_ADAPTER_IFACE) != 0) return;
    if (s->adapter_path[0] != '\0') return; /* first adapter wins */
    DBusMessageIter v;
    rb_copy_str(s->adapter_path, sizeof(s->adapter_path), path);
    if (rb_dbus_dict_find(props, "Address", &v))
        rb_dbus_iter_string(&v, s->adapter_address,
                            sizeof(s->adapter_address));
    if (rb_dbus_dict_find(props, "Powered", &v)) {
        int p = 0;
        if (rb_dbus_iter_bool(&v, &p)) s->adapter_powered = p;
    }
}

static rb_status_t linux_create(rb_context_t *ctx) {
    char detail[256] = "";
    rb_dbus_conn_t *bus = NULL;
    rb_status_t st = rb_dbus_open(&bus, detail, sizeof(detail));
    if (st != RB_OK) {
        rb_logf(ctx, RB_LOG_WARN, "linux", "bus open failed: %s", detail);
        return RB_ERROR_UNAVAILABLE;
    }
    char d2[256] = "";
    int bluez = rb_dbus_name_has_owner(bus, RB_BLUEZ_SERVICE, d2,
                                       sizeof(d2)) == 1;
    int nm = rb_dbus_name_has_owner(bus, RB_NM_SERVICE, d2,
                                    sizeof(d2)) == 1;
    if (!bluez && !nm) {
        rb_dbus_close(bus);
        rb_logf(ctx, RB_LOG_WARN, "linux",
                "neither org.bluez nor org.freedesktop.NetworkManager "
                "on the bus; backend unavailable");
        return RB_ERROR_UNAVAILABLE;
    }
    linux_state_t *s = (linux_state_t *)calloc(1, sizeof(*s));
    if (!s) {
        rb_dbus_close(bus);
        return RB_ERROR_NO_MEMORY;
    }
    s->ctx = ctx;
    s->bus = bus;
    s->bluez_present = bluez;
    s->nm_present = nm;
    s->adapter_powered = -1;
    rb_mutex_init(&s->lock);
    if (bluez) {
        char en[256], dt[256];
        DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, "/",
                                            RB_DBUS_OBJECT_MANAGER,
                                            "GetManagedObjects");
        if (req) {
            DBusMessage *rep =
                rb_dbus_call(bus, req, 5000, en, sizeof(en), dt,
                             sizeof(dt));
            if (rep) {
                rb_dbus_walk_managed_objects(rep, linux_find_adapter_cb,
                                             s);
                dbus_message_unref(rep);
            }
        }
        if (s->adapter_path[0] == '\0')
            rb_copy_str(s->adapter_path, sizeof(s->adapter_path),
                        "/org/bluez/hci0");
    }
    if (nm) {
        char en[256], dt[256];
        nm_snapshot_t snap;
        if (nm_snapshot_take(bus, &snap, en, sizeof(en), dt,
                             sizeof(dt))) {
            rb_copy_str(s->nm_version, sizeof(s->nm_version),
                        snap.version);
            const nm_device_snap_t *w = nm_find_wifi(&snap);
            if (w)
                rb_copy_str(s->wifi_device_path,
                            sizeof(s->wifi_device_path), w->path);
        }
    }
    ctx->backend_state = s;
    rb_logf(ctx, RB_LOG_INFO, "linux",
            "backend up: BlueZ %s, NetworkManager %s, adapter %s",
            bluez ? "present" : "absent", nm ? "present" : "absent",
            s->adapter_path);
    return RB_OK;
}

static void linux_stop_sub(linux_state_t *s, linux_sub_t *sub) {
    (void)s;
    if (!sub->active) return;
    sub->stop = 1;
    if (!rb_thread_equal(rb_thread_self(), sub->thread))
        rb_thread_join(sub->thread);
    sub->active = 0;
}

static void linux_destroy(rb_context_t *ctx) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return;
    /* Stop scans. */
    rb_mutex_lock(&s->lock);
    int scan_active = s->scan_active;
    int wifi_active = s->wifi_scan_active;
    if (scan_active) s->scan_stop = 1;
    if (wifi_active) s->wifi_scan_stop = 1;
    rb_mutex_unlock(&s->lock);
    if (scan_active &&
        !rb_thread_equal(rb_thread_self(), s->scan_thread))
        rb_thread_join(s->scan_thread);
    if (wifi_active &&
        !rb_thread_equal(rb_thread_self(), s->wifi_scan_thread))
        rb_thread_join(s->wifi_scan_thread);
    for (int i = 0; i < RB_LINUX_MAX_SUBS; i++)
        linux_stop_sub(s, &s->subs[i]);
    rb_dbus_close(s->bus);
    rb_mutex_destroy(&s->lock);
    free(s);
    ctx->backend_state = NULL;
}

static rb_status_t linux_diagnostics(rb_context_t *ctx,
                                     rb_diagnostics_t *out) {
    linux_state_t *s = linux_get(ctx);
    if (!s || !out) return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&s->lock);
    rb_copy_str(out->bus_address, sizeof(out->bus_address),
                rb_dbus_address(s->bus));
    out->bluez_present = s->bluez_present;
    out->nm_present = s->nm_present;
    rb_copy_str(out->nm_version, sizeof(out->nm_version), s->nm_version);
    rb_copy_str(out->adapter_path, sizeof(out->adapter_path),
                s->adapter_path);
    rb_copy_str(out->adapter_address, sizeof(out->adapter_address),
                s->adapter_address);
    out->adapter_powered = s->adapter_powered;
    rb_copy_str(out->wifi_device_path, sizeof(out->wifi_device_path),
                s->wifi_device_path);
    rb_mutex_unlock(&s->lock);
    rb_copy_str(out->compatibility, sizeof(out->compatibility),
                "Linux v0.2: scan is snapshot-based (StartDiscovery + "
                "GetManagedObjects); GATT handles are synthetic "
                "sequential indexes (BlueZ exposes none); NM signal "
                "is percent, reported as signal_dbm=0 (not reported); "
                "AP security: RsnFlags->WPA2 (WPA3/SAE not "
                "distinguished), WpaFlags->WPA, privacy bit->WEP; "
                "notifications are poll-based (~150 ms) after "
                "StartNotify; Wi-Fi connect uses key-mgmt wpa-psk "
                "(WPA2+WPA3 transition), 802.1X enterprise "
                "unsupported; auto-reconnect stored but not "
                "attempted; no WIFI_CHANGED events in v0.2 (poll "
                "status); quality is a TCP-connect probe, not ICMP "
                "or radio metrics.");
    rb_copy_str(out->detail, sizeof(out->detail),
                s->bluez_present || s->nm_present
                    ? "linux backend live on D-Bus"
                    : "linux backend without bus services");
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* BLE operations                                                      */
/* ------------------------------------------------------------------ */

static int device_matches(const bt_device_snap_t *d,
                          const rb_ble_scan_options_t *opts) {
    if (!opts) return 1;
    if (opts->service_uuid_count > 0) {
        int found = 0;
        for (size_t i = 0; i < opts->service_uuid_count && !found; i++)
            for (size_t j = 0; j < d->uuid_count; j++)
                if (rb_uuid_equals(&opts->service_uuids[i],
                                   &d->uuids[j])) {
                    found = 1;
                    break;
                }
        if (!found) return 0;
    }
    if (opts->manufacturer_id >= 0) {
        if (!d->has_mfr ||
            d->mfr_id != (uint16_t)opts->manufacturer_id)
            return 0;
    }
    if (opts->has_min_rssi) {
        if (!d->has_rssi || d->rssi < opts->min_rssi_dbm) return 0;
    }
    if (opts->name_substring[0] != '\0') {
        const char *name = d->name[0] ? d->name : d->alias;
        if (!contains_ci(name, opts->name_substring)) return 0;
    }
    return 1;
}

static void snap_to_device(const bt_device_snap_t *d,
                           rb_ble_device_t *out) {
    memset(out, 0, sizeof(*out));
    rb_copy_str(out->address, sizeof(out->address), d->address);
    rb_copy_str(out->name, sizeof(out->name),
                d->name[0] ? d->name : d->alias);
    out->rssi_dbm = d->has_rssi ? (int8_t)d->rssi : 0;
    out->has_tx_power = d->has_tx_power;
    out->tx_power_dbm = d->has_tx_power ? (int8_t)d->tx_power : 0;
    /* BlueZ exposes no connectability; discovered devices are
     * reported connectable (documented). */
    out->connectable = 1;
    out->service_uuid_count = d->uuid_count;
    for (size_t i = 0; i < d->uuid_count; i++)
        out->service_uuids[i] = d->uuids[i];
    out->has_manufacturer_data = d->has_mfr;
    out->manufacturer_id = d->mfr_id;
    out->manufacturer_data_len = d->mfr_len;
    if (d->mfr_len > 0)
        memcpy(out->manufacturer_data, d->mfr_data, d->mfr_len);
}

typedef struct scan_job {
    rb_context_t *ctx;
    rb_ble_scan_options_t opts;
    rb_ble_scan_callback_t cb;
    void *ud;
    rb_operation_id_t op;
} scan_job_t;

static void *linux_scan_thread(void *arg) {
    scan_job_t *job = (scan_job_t *)arg;
    linux_state_t *s = linux_get(job->ctx);
    char en[256], dt[256];
    /* StartDiscovery; an InProgress error means another client is
     * already discovering — the snapshot still works, so proceed. */
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, s->adapter_path,
                                        RB_BLUEZ_ADAPTER_IFACE,
                                        "StartDiscovery");
    if (req) {
        DBusMessage *rep =
            rb_dbus_call(s->bus, req, 5000, en, sizeof(en), dt,
                         sizeof(dt));
        if (rep) dbus_message_unref(rep);
    }
    char seen[RB_LINUX_MAX_DEVICES][32];
    size_t seen_count = 0;
    uint64_t start = rb_now_ms();
    for (;;) {
        rb_mutex_lock(&s->lock);
        int stop = s->scan_stop;
        rb_mutex_unlock(&s->lock);
        if (stop) break;
        if (job->opts.timeout_ms > 0 &&
            rb_now_ms() - start >= job->opts.timeout_ms)
            break;
        bt_snapshot_t snap;
        if (bt_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                             sizeof(dt))) {
            for (size_t i = 0; i < snap.device_count; i++) {
                const bt_device_snap_t *d = &snap.devices[i];
                if (!device_matches(d, &job->opts)) continue;
                if (!job->opts.allow_duplicates) {
                    int dup = 0;
                    for (size_t k = 0; k < seen_count; k++)
                        if (strcmp(seen[k], d->address) == 0) {
                            dup = 1;
                            break;
                        }
                    if (dup) continue;
                    if (seen_count < RB_LINUX_MAX_DEVICES)
                        rb_copy_str(seen[seen_count++], 32, d->address);
                }
                rb_ble_device_t dev;
                snap_to_device(d, &dev);
                if (job->cb) job->cb(job->ctx, &dev, job->ud);
            }
        }
        /* Settle before the first pass is folded into this loop: the
         * first snapshot follows StartDiscovery immediately, then we
         * sleep between passes. A short initial settle improves the
         * first pass on real adapters. */
        uint64_t elapsed = rb_now_ms() - start;
        if (elapsed < 300) {
            rb_sleep_ms((unsigned int)(300 - elapsed));
            continue;
        }
        for (int waited = 0; waited < 1000; waited += 50) {
            rb_mutex_lock(&s->lock);
            stop = s->scan_stop;
            rb_mutex_unlock(&s->lock);
            if (stop) break;
            rb_sleep_ms(50);
        }
        if (job->opts.timeout_ms == 0) {
            /* Snapshot loop runs until stopped; keep going. */
        } else if (rb_now_ms() - start >= job->opts.timeout_ms) {
            break;
        }
    }
    req = rb_dbus_new_call(RB_BLUEZ_SERVICE, s->adapter_path,
                           RB_BLUEZ_ADAPTER_IFACE, "StopDiscovery");
    if (req) {
        DBusMessage *rep =
            rb_dbus_call(s->bus, req, 5000, en, sizeof(en), dt,
                         sizeof(dt));
        if (rep) dbus_message_unref(rep);
    }
    rb_mutex_lock(&s->lock);
    s->scan_active = 0;
    rb_mutex_unlock(&s->lock);
    rb_op_mark_done(job->ctx, job->op);
    free(job);
    return NULL;
}

static rb_status_t linux_ble_scan_start(rb_context_t *ctx,
                                        const rb_ble_scan_options_t *opts,
                                        rb_ble_scan_callback_t cb,
                                        void *ud, rb_operation_id_t op) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    rb_mutex_lock(&s->lock);
    if (s->scan_active) {
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_ALREADY_IN_PROGRESS;
    }
    s->scan_active = 1;
    s->scan_stop = 0;
    s->scan_op = op;
    rb_mutex_unlock(&s->lock);
    scan_job_t *job = (scan_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        rb_mutex_lock(&s->lock);
        s->scan_active = 0;
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_NO_MEMORY;
    }
    job->ctx = ctx;
    if (opts) job->opts = *opts;
    job->cb = cb;
    job->ud = ud;
    job->op = op;
    if (rb_thread_create(&s->scan_thread, linux_scan_thread, job) != 0) {
        free(job);
        rb_mutex_lock(&s->lock);
        s->scan_active = 0;
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_INTERNAL;
    }
    return RB_OK;
}

static rb_status_t linux_ble_scan_stop(rb_context_t *ctx,
                                       rb_operation_id_t op) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    (void)op;
    rb_mutex_lock(&s->lock);
    if (!s->scan_active) {
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_NOT_FOUND;
    }
    s->scan_stop = 1;
    rb_thread_t th = s->scan_thread;
    rb_mutex_unlock(&s->lock);
    if (!rb_thread_equal(rb_thread_self(), th)) rb_thread_join(th);
    return RB_OK;
}

typedef struct connect_job {
    rb_context_t *ctx;
    char address[32];
    uint32_t timeout_ms;
    rb_ble_connection_callback_t cb;
    void *ud;
    rb_operation_id_t op;
} connect_job_t;

static void *linux_connect_thread(void *arg);

static rb_status_t linux_ble_connect(rb_context_t *ctx,
                                     const rb_device_id_t *dev,
                                     const rb_ble_connect_options_t *opts,
                                     rb_ble_connection_callback_t cb,
                                     void *ud, rb_operation_id_t op) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !rb_address_valid(dev->address))
        return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    (void)opts;
    /* Asynchronous like the mock: the worker does the D-Bus work and
     * reports the outcome via the callback; unknown addresses report
     * FAILED/NOT_FOUND asynchronously (public contract). CONNECTING
     * is emitted synchronously, before returning OK. */
    connect_job_t *job = (connect_job_t *)calloc(1, sizeof(*job));
    if (!job) return RB_ERROR_NO_MEMORY;
    job->ctx = ctx;
    rb_copy_str(job->address, sizeof(job->address), dev->address);
    job->timeout_ms = opts && opts->timeout_ms ? opts->timeout_ms : 10000;
    job->cb = cb;
    job->ud = ud;
    job->op = op;
    rb_thread_t th;
    if (rb_thread_create(&th, linux_connect_thread, job) != 0) {
        free(job);
        return RB_ERROR_INTERNAL;
    }
    rb_thread_detach(th);
    if (cb) {
        rb_ble_connection_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.device = *dev;
        ev.state = RB_BLE_CONNECTING;
        ev.status = RB_OK;
        cb(ctx, &ev, ud);
    }
    return RB_OK;
}

static void *linux_connect_thread(void *arg) {
    connect_job_t *job = (connect_job_t *)arg;
    linux_state_t *s = linux_get(job->ctx);
    char en[256], dt[256];
    rb_device_id_t dev;
    memset(&dev, 0, sizeof(dev));
    rb_copy_str(dev.address, sizeof(dev.address), job->address);
    rb_status_t final_st = RB_OK;
    char path[256] = "";
    bt_snapshot_t snap;
    if (!bt_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt))) {
        final_st = rb_dbus_error_to_status(en);
    } else {
        const bt_device_snap_t *d = bt_find_device(&snap, job->address);
        if (!d) {
            final_st = RB_ERROR_NOT_FOUND;
        } else {
            rb_copy_str(path, sizeof(path), d->path);
        }
    }
    if (final_st == RB_OK) {
        DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, path,
                                            RB_BLUEZ_DEVICE_IFACE,
                                            "Connect");
        if (!req) {
            final_st = RB_ERROR_NO_MEMORY;
        } else {
            DBusMessage *rep = rb_dbus_call(s->bus, req, 30000, en,
                                            sizeof(en), dt, sizeof(dt));
            if (!rep) {
                final_st = rb_dbus_error_to_status(en);
            } else {
                dbus_message_unref(rep);
                uint64_t start = rb_now_ms();
                int connected = 0;
                while (rb_now_ms() - start < job->timeout_ms) {
                    DBusMessage *pr = rb_dbus_get_property(
                        s->bus, RB_BLUEZ_SERVICE, path,
                        RB_BLUEZ_DEVICE_IFACE, "Connected", en,
                        sizeof(en), dt, sizeof(dt));
                    if (pr) {
                        int c = 0;
                        if (rb_dbus_variant_bool(pr, &c) && c)
                            connected = 1;
                        dbus_message_unref(pr);
                        if (connected) break;
                    }
                    rb_sleep_ms(200);
                }
                if (!connected) final_st = RB_ERROR_TIMEOUT;
            }
        }
    }
    if (job->cb) {
        rb_ble_connection_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.device = dev;
        ev.state =
            final_st == RB_OK ? RB_BLE_CONNECTED : RB_BLE_FAILED;
        ev.status = final_st;
        if (final_st != RB_OK)
            rb_copy_str(ev.message, sizeof(ev.message), dt);
        job->cb(job->ctx, &ev, job->ud);
    }
    rb_op_mark_done(job->ctx, job->op);
    free(job);
    return NULL;
}

static rb_status_t linux_ble_disconnect(rb_context_t *ctx,
                                        const rb_device_id_t *dev) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !rb_address_valid(dev->address))
        return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    char en[256], dt[256];
    bt_snapshot_t snap;
    if (!bt_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt)))
        return rb_dbus_error_to_status(en);
    const bt_device_snap_t *d = bt_find_device(&snap, dev->address);
    if (!d) return RB_ERROR_NOT_FOUND;
    if (!d->connected) return RB_ERROR_NOT_CONNECTED;
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, d->path,
                                        RB_BLUEZ_DEVICE_IFACE,
                                        "Disconnect");
    if (!req) return RB_ERROR_NO_MEMORY;
    DBusMessage *rep =
        rb_dbus_call(s->bus, req, 10000, en, sizeof(en), dt, sizeof(dt));
    if (!rep) return rb_dbus_error_to_status(en);
    dbus_message_unref(rep);
    return RB_OK;
}

/* Resolve a GATT characteristic path from device address + UUIDs. */
static rb_status_t linux_resolve_char(linux_state_t *s,
                                      const rb_device_id_t *dev,
                                      const rb_uuid_t *svc,
                                      const rb_uuid_t *chr,
                                      char *char_path, size_t char_path_len,
                                      char *svc_path, size_t svc_path_len) {
    char en[256], dt[256];
    bt_snapshot_t snap;
    if (!bt_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt)))
        return rb_dbus_error_to_status(en);
    const bt_device_snap_t *d = bt_find_device(&snap, dev->address);
    if (!d) return RB_ERROR_NOT_FOUND;
    const bt_service_snap_t *service = NULL;
    for (size_t i = 0; i < snap.service_count; i++) {
        if (strcmp(snap.services[i].device_path, d->path) != 0) continue;
        if (rb_uuid_equals(&snap.services[i].uuid, svc)) {
            service = &snap.services[i];
            break;
        }
    }
    if (!service) return RB_ERROR_NOT_FOUND;
    for (size_t i = 0; i < snap.char_count; i++) {
        if (strcmp(snap.chars[i].service_path, service->path) != 0)
            continue;
        if (rb_uuid_equals(&snap.chars[i].uuid, chr)) {
            rb_copy_str(char_path, char_path_len, snap.chars[i].path);
            if (svc_path)
                rb_copy_str(svc_path, svc_path_len, service->path);
            return RB_OK;
        }
    }
    return RB_ERROR_NOT_FOUND;
}

static uint32_t gatt_flags_to_bits(const bt_char_snap_t *c) {
    uint32_t bits = 0;
    for (size_t i = 0; i < c->flag_count; i++) {
        const char *f = c->flags[i];
        if (strcmp(f, "broadcast") == 0) bits |= RB_GATT_PROP_BROADCAST;
        else if (strcmp(f, "read") == 0) bits |= RB_GATT_PROP_READ;
        else if (strcmp(f, "write-without-response") == 0)
            bits |= RB_GATT_PROP_WRITE_NO_RSP;
        else if (strcmp(f, "write") == 0) bits |= RB_GATT_PROP_WRITE;
        else if (strcmp(f, "notify") == 0) bits |= RB_GATT_PROP_NOTIFY;
        else if (strcmp(f, "indicate") == 0) bits |= RB_GATT_PROP_INDICATE;
    }
    return bits;
}

static rb_status_t linux_ble_discover(rb_context_t *ctx,
                                      const rb_device_id_t *dev,
                                      rb_ble_service_t **out,
                                      size_t *count) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !out || !count) return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    *out = NULL;
    *count = 0;
    char en[256], dt[256];
    bt_snapshot_t snap;
    if (!bt_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt)))
        return rb_dbus_error_to_status(en);
    const bt_device_snap_t *d = bt_find_device(&snap, dev->address);
    if (!d) return RB_ERROR_NOT_FOUND;
    if (!d->services_resolved) return RB_ERROR_NOT_CONNECTED;
    /* Count services for this device. */
    size_t nsvc = 0;
    for (size_t i = 0; i < snap.service_count; i++)
        if (strcmp(snap.services[i].device_path, d->path) == 0) nsvc++;
    rb_ble_service_t *arr =
        (rb_ble_service_t *)calloc(nsvc ? nsvc : 1, sizeof(*arr));
    if (!arr) return RB_ERROR_NO_MEMORY;
    size_t si = 0;
    for (size_t i = 0; i < snap.service_count && si < nsvc; i++) {
        const bt_service_snap_t *ss = &snap.services[i];
        if (strcmp(ss->device_path, d->path) != 0) continue;
        rb_ble_service_t *svc = &arr[si];
        svc->uuid = ss->uuid;
        svc->handle = (uint16_t)(i + 1); /* synthetic, snapshot order */
        size_t ci = 0;
        for (size_t j = 0; j < snap.char_count &&
                           ci < RB_BLE_MAX_CHARS_PER_SERVICE;
             j++) {
            const bt_char_snap_t *cc = &snap.chars[j];
            if (strcmp(cc->service_path, ss->path) != 0) continue;
            rb_ble_characteristic_t *ch =
                &svc->characteristics[svc->characteristic_count++];
            ch->uuid = cc->uuid;
            ch->handle = (uint16_t)(j + 1); /* synthetic */
            ch->properties = gatt_flags_to_bits(cc);
            size_t di = 0;
            for (size_t k = 0; k < snap.desc_count &&
                               di < RB_BLE_MAX_DESCS_PER_CHAR;
                 k++) {
                const bt_desc_snap_t *dd = &snap.descs[k];
                if (strcmp(dd->char_path, cc->path) != 0) continue;
                rb_ble_descriptor_t *desc =
                    &ch->descriptors[ch->descriptor_count++];
                desc->uuid = dd->uuid;
                desc->handle = (uint16_t)(k + 1); /* synthetic */
                di++;
            }
            ci++;
        }
        si++;
    }
    *out = arr;
    *count = nsvc;
    return RB_OK;
}

static rb_status_t linux_ble_read(rb_context_t *ctx,
                                  const rb_device_id_t *dev,
                                  const rb_uuid_t *svc,
                                  const rb_uuid_t *chr, uint8_t *out_value,
                                  size_t *out_len) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !svc || !chr || !out_value || !out_len)
        return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    char char_path[256];
    rb_status_t st =
        linux_resolve_char(s, dev, svc, chr, char_path,
                           sizeof(char_path), NULL, 0);
    if (st != RB_OK) return st;
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, char_path,
                                        RB_BLUEZ_GATT_CHAR_IFACE,
                                        "ReadValue");
    if (!req) return RB_ERROR_NO_MEMORY;
    DBusMessageIter it, dict;
    rb_dbus_append_iter(req, &it);
    rb_dbus_open_dict(&it, &dict);
    rb_dbus_close_dict(&it, &dict);
    char en[256], dt[256];
    DBusMessage *rep =
        rb_dbus_call(s->bus, req, 10000, en, sizeof(en), dt, sizeof(dt));
    if (!rep) return rb_dbus_error_to_status(en);
    DBusMessageIter rit;
    size_t got = 0;
    int ok = 0;
    if (dbus_message_iter_init(rep, &rit))
        ok = rb_dbus_iter_byte_array(&rit, out_value,
                                     RB_BLE_MAX_VALUE_LEN, &got);
    dbus_message_unref(rep);
    if (!ok) return RB_ERROR_INTERNAL;
    *out_len = got;
    return RB_OK;
}

static rb_status_t linux_ble_write(rb_context_t *ctx,
                                   const rb_device_id_t *dev,
                                   const rb_uuid_t *svc,
                                   const rb_uuid_t *chr,
                                   const uint8_t *value, size_t len,
                                   int with_response) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !svc || !chr || (!value && len > 0))
        return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    char char_path[256];
    rb_status_t st =
        linux_resolve_char(s, dev, svc, chr, char_path,
                           sizeof(char_path), NULL, 0);
    if (st != RB_OK) return st;
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, char_path,
                                        RB_BLUEZ_GATT_CHAR_IFACE,
                                        "WriteValue");
    if (!req) return RB_ERROR_NO_MEMORY;
    rb_dbus_append_byte_array(req, value, len);
    DBusMessageIter it, dict;
    rb_dbus_append_iter(req, &it);
    rb_dbus_open_dict(&it, &dict);
    rb_dbus_dict_put_string(&dict, "type",
                            with_response ? "request" : "command");
    rb_dbus_close_dict(&it, &dict);
    char en[256], dt[256];
    DBusMessage *rep =
        rb_dbus_call(s->bus, req, 10000, en, sizeof(en), dt, sizeof(dt));
    if (!rep) return rb_dbus_error_to_status(en);
    dbus_message_unref(rep);
    return RB_OK;
}

typedef struct notify_job {
    linux_state_t *s;
    linux_sub_t *sub;
} notify_job_t;

static void *linux_notify_thread(void *arg) {
    notify_job_t *job = (notify_job_t *)arg;
    linux_state_t *s = job->s;
    linux_sub_t *sub = job->sub;
    free(job);
    for (;;) {
        rb_mutex_lock(&s->lock);
        int stop = sub->stop;
        rb_mutex_unlock(&s->lock);
        if (stop) break;
        char en[256], dt[256];
        DBusMessage *pr = rb_dbus_get_property(
            s->bus, RB_BLUEZ_SERVICE, sub->char_path,
            RB_BLUEZ_GATT_CHAR_IFACE, "Value", en, sizeof(en), dt,
            sizeof(dt));
        if (pr) {
            uint8_t buf[RB_BLE_MAX_VALUE_LEN];
            size_t len = 0;
            if (rb_dbus_variant_byte_array(pr, buf, sizeof(buf), &len)) {
                int changed = 0;
                rb_mutex_lock(&s->lock);
                if (!sub->have_last) {
                    /* Baseline: first value is not a notification. */
                    sub->have_last = 1;
                    memcpy(sub->last, buf, len);
                    sub->last_len = len;
                } else if (sub->last_len != len ||
                           (len > 0 &&
                            memcmp(sub->last, buf, len) != 0)) {
                    changed = 1;
                    memcpy(sub->last, buf, len);
                    sub->last_len = len;
                }
                rb_ble_notify_callback_t cb = sub->cb;
                void *ud = sub->ud;
                rb_mutex_unlock(&s->lock);
                if (changed && cb) {
                    rb_ble_notification_t n;
                    memset(&n, 0, sizeof(n));
                    rb_copy_str(n.device.address,
                                sizeof(n.device.address),
                                sub->device_address);
                    n.service_uuid = sub->svc_uuid;
                    n.characteristic_uuid = sub->chr_uuid;
                    memcpy(n.data, buf, len);
                    n.data_len = len;
                    cb(s->ctx, &n, ud);
                }
            }
            dbus_message_unref(pr);
        }
        for (int waited = 0; waited < 150; waited += 25) {
            rb_mutex_lock(&s->lock);
            stop = sub->stop;
            rb_mutex_unlock(&s->lock);
            if (stop) break;
            rb_sleep_ms(25);
        }
    }
    return NULL;
}

static rb_status_t linux_ble_subscribe(rb_context_t *ctx,
                                       const rb_device_id_t *dev,
                                       const rb_uuid_t *svc,
                                       const rb_uuid_t *chr,
                                       rb_ble_notify_callback_t cb,
                                       void *ud) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !svc || !chr || !cb) return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    char char_path[256];
    rb_status_t st =
        linux_resolve_char(s, dev, svc, chr, char_path,
                           sizeof(char_path), NULL, 0);
    if (st != RB_OK) return st;
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, char_path,
                                        RB_BLUEZ_GATT_CHAR_IFACE,
                                        "StartNotify");
    if (!req) return RB_ERROR_NO_MEMORY;
    DBusMessageIter it, dict;
    rb_dbus_append_iter(req, &it);
    rb_dbus_open_dict(&it, &dict);
    rb_dbus_close_dict(&it, &dict);
    char en[256], dt[256];
    DBusMessage *rep =
        rb_dbus_call(s->bus, req, 10000, en, sizeof(en), dt, sizeof(dt));
    if (!rep) return rb_dbus_error_to_status(en);
    dbus_message_unref(rep);
    rb_mutex_lock(&s->lock);
    linux_sub_t *slot = NULL;
    for (int i = 0; i < RB_LINUX_MAX_SUBS; i++) {
        if (!s->subs[i].active) {
            slot = &s->subs[i];
            break;
        }
    }
    if (!slot) {
        rb_mutex_unlock(&s->lock);
        /* Roll back: stop notify on the device. */
        req = rb_dbus_new_call(RB_BLUEZ_SERVICE, char_path,
                               RB_BLUEZ_GATT_CHAR_IFACE, "StopNotify");
        if (req) {
            DBusMessageIter it2, dict2;
            rb_dbus_append_iter(req, &it2);
            rb_dbus_open_dict(&it2, &dict2);
            rb_dbus_close_dict(&it2, &dict2);
            rep = rb_dbus_call(s->bus, req, 5000, en, sizeof(en), dt,
                               sizeof(dt));
            if (rep) dbus_message_unref(rep);
        }
        return RB_ERROR_BUSY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    rb_copy_str(slot->device_address, sizeof(slot->device_address),
                dev->address);
    rb_copy_str(slot->char_path, sizeof(slot->char_path), char_path);
    slot->svc_uuid = *svc;
    slot->chr_uuid = *chr;
    slot->cb = cb;
    slot->ud = ud;
    notify_job_t *job = (notify_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        slot->active = 0;
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_NO_MEMORY;
    }
    job->s = s;
    job->sub = slot;
    if (rb_thread_create(&slot->thread, linux_notify_thread, job) != 0) {
        free(job);
        slot->active = 0;
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_INTERNAL;
    }
    rb_mutex_unlock(&s->lock);
    return RB_OK;
}

static rb_status_t linux_ble_unsubscribe(rb_context_t *ctx,
                                         const rb_device_id_t *dev,
                                         const rb_uuid_t *svc,
                                         const rb_uuid_t *chr) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !svc || !chr) return RB_ERROR_INVALID_ARGUMENT;
    rb_mutex_lock(&s->lock);
    linux_sub_t *slot = NULL;
    for (int i = 0; i < RB_LINUX_MAX_SUBS; i++) {
        if (s->subs[i].active &&
            strcmp(s->subs[i].device_address, dev->address) == 0 &&
            rb_uuid_equals(&s->subs[i].svc_uuid, svc) &&
            rb_uuid_equals(&s->subs[i].chr_uuid, chr)) {
            slot = &s->subs[i];
            break;
        }
    }
    if (!slot) {
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_NOT_FOUND;
    }
    char char_path[256];
    rb_copy_str(char_path, sizeof(char_path), slot->char_path);
    rb_mutex_unlock(&s->lock);
    linux_stop_sub(s, slot);
    char en[256], dt[256];
    DBusMessage *req = rb_dbus_new_call(RB_BLUEZ_SERVICE, char_path,
                                        RB_BLUEZ_GATT_CHAR_IFACE,
                                        "StopNotify");
    if (req) {
        DBusMessageIter it, dict;
        rb_dbus_append_iter(req, &it);
        rb_dbus_open_dict(&it, &dict);
        rb_dbus_close_dict(&it, &dict);
        DBusMessage *rep =
            rb_dbus_call(s->bus, req, 5000, en, sizeof(en), dt,
                         sizeof(dt));
        if (rep) dbus_message_unref(rep);
    }
    return RB_OK;
}

static rb_status_t linux_ble_get_rssi(rb_context_t *ctx,
                                      const rb_device_id_t *dev,
                                      int8_t *out_rssi_dbm) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!dev || !out_rssi_dbm) return RB_ERROR_INVALID_ARGUMENT;
    if (!s->bluez_present) return RB_ERROR_UNAVAILABLE;
    char en[256], dt[256];
    bt_snapshot_t snap;
    if (!bt_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt)))
        return rb_dbus_error_to_status(en);
    const bt_device_snap_t *d = bt_find_device(&snap, dev->address);
    if (!d) return RB_ERROR_NOT_FOUND;
    DBusMessage *pr = rb_dbus_get_property(s->bus, RB_BLUEZ_SERVICE,
                                           d->path, RB_BLUEZ_DEVICE_IFACE,
                                           "RSSI", en, sizeof(en), dt,
                                           sizeof(dt));
    if (!pr) return rb_dbus_error_to_status(en);
    int16_t r = 0;
    int ok = rb_dbus_variant_int16(pr, &r);
    dbus_message_unref(pr);
    if (!ok) return RB_ERROR_UNAVAILABLE; /* RSSI property absent */
    *out_rssi_dbm = (int8_t)r;
    return RB_OK;
}

static rb_status_t linux_ble_set_reconnect_policy(
    rb_context_t *ctx, const rb_reconnect_policy_t *p) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!p) return RB_ERROR_INVALID_ARGUMENT;
    /* Stored; auto-reconnect is not attempted on Linux in v0.2
     * (documented — see the file header). */
    rb_mutex_lock(&s->lock);
    s->reconnect_policy = *p;
    rb_mutex_unlock(&s->lock);
    return RB_OK;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi operations                                                    */
/* ------------------------------------------------------------------ */

static rb_status_t linux_wifi_get_status(rb_context_t *ctx,
                                         rb_wifi_status_t *out) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!out) return RB_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!s->nm_present) {
        out->state = RB_WIFI_UNAVAILABLE;
        out->diag = RB_WIFI_DIAG_UNAVAILABLE;
        rb_copy_str(out->message, sizeof(out->message),
                    "NetworkManager service absent");
        return RB_OK;
    }
    char en[256], dt[256];
    nm_snapshot_t snap;
    if (!nm_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt))) {
        out->state = RB_WIFI_UNAVAILABLE;
        out->diag = RB_WIFI_DIAG_UNAVAILABLE;
        rb_copy_str(out->message, sizeof(out->message), dt);
        return RB_OK;
    }
    const nm_device_snap_t *w = nm_find_wifi(&snap);
    if (!w) {
        out->state = RB_WIFI_UNAVAILABLE;
        out->diag = RB_WIFI_DIAG_UNAVAILABLE;
        rb_copy_str(out->message, sizeof(out->message),
                    "no Wi-Fi device on this host");
        return RB_OK;
    }
    out->state = nm_state_to_wifi(w->state, &out->diag);
    /* NM reports percent, not dBm: signal_dbm stays 0 (not reported). */
    out->signal_dbm = 0;
    if (w->active_ap[0] && strcmp(w->active_ap, "/") != 0) {
        const nm_ap_snap_t *ap = nm_find_ap(&snap, w->active_ap);
        if (ap) {
            size_t n = ap->ssid_len < sizeof(out->ssid) - 1
                           ? ap->ssid_len
                           : sizeof(out->ssid) - 1;
            memcpy(out->ssid, ap->ssid, n);
            out->ssid[n] = '\0';
            rb_copy_str(out->bssid, sizeof(out->bssid), ap->hw_address);
        }
    }
    if (w->ip4config[0] && strcmp(w->ip4config, "/") != 0) {
        const nm_ip4_snap_t *ip = nm_find_ip4(&snap, w->ip4config);
        if (ip) {
            rb_copy_str(out->ip_address, sizeof(out->ip_address),
                        ip->address);
            rb_copy_str(out->gateway, sizeof(out->gateway), ip->gateway);
        }
    }
    rb_copy_str(out->message, sizeof(out->message),
                out->state == RB_WIFI_CONNECTED
                    ? "connected"
                    : "see state/diag");
    return RB_OK;
}

typedef struct wifi_scan_job {
    rb_context_t *ctx;
    rb_wifi_scan_callback_t cb;
    void *ud;
    rb_operation_id_t op;
} wifi_scan_job_t;

static void *linux_wifi_scan_thread(void *arg) {
    wifi_scan_job_t *job = (wifi_scan_job_t *)arg;
    linux_state_t *s = linux_get(job->ctx);
    char en[256], dt[256];
    /* RequestScan (best effort — proceed to the snapshot even if the
     * scan request itself fails). */
    DBusMessage *req = rb_dbus_new_call(RB_NM_SERVICE, s->wifi_device_path,
                                        RB_NM_WIRELESS_IFACE,
                                        "RequestScan");
    if (req) {
        DBusMessageIter it, dict;
        rb_dbus_append_iter(req, &it);
        rb_dbus_open_dict(&it, &dict);
        rb_dbus_close_dict(&it, &dict);
        DBusMessage *rep =
            rb_dbus_call(s->bus, req, 5000, en, sizeof(en), dt,
                         sizeof(dt));
        if (rep) dbus_message_unref(rep);
    }
    /* GetAccessPoints gives paths; the snapshot gives properties. */
    char *paths[RB_LINUX_MAX_APS];
    size_t path_count = 0;
    req = rb_dbus_new_call(RB_NM_SERVICE, s->wifi_device_path,
                           RB_NM_WIRELESS_IFACE, "GetAccessPoints");
    if (req) {
        DBusMessage *rep =
            rb_dbus_call(s->bus, req, 5000, en, sizeof(en), dt,
                         sizeof(dt));
        if (rep) {
            DBusMessageIter it, arr;
            if (dbus_message_iter_init(rep, &it) &&
                dbus_message_iter_get_arg_type(&it) ==
                    DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&it, &arr);
                while (dbus_message_iter_get_arg_type(&arr) ==
                           DBUS_TYPE_OBJECT_PATH &&
                       path_count < RB_LINUX_MAX_APS) {
                    const char *p = NULL;
                    dbus_message_iter_get_basic(&arr, &p);
                    if (p) {
                        size_t n = strlen(p) + 1;
                        paths[path_count] = (char *)malloc(n);
                        if (paths[path_count]) {
                            memcpy(paths[path_count], p, n);
                            path_count++;
                        }
                    }
                    dbus_message_iter_next(&arr);
                }
            }
            dbus_message_unref(rep);
        }
    }
    nm_snapshot_t snap;
    if (nm_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                         sizeof(dt))) {
        for (size_t i = 0; i < path_count; i++) {
            const nm_ap_snap_t *ap = nm_find_ap(&snap, paths[i]);
            if (!ap) continue;
            rb_wifi_network_t net;
            memset(&net, 0, sizeof(net));
            size_t n = ap->ssid_len < sizeof(net.ssid) - 1
                           ? ap->ssid_len
                           : sizeof(net.ssid) - 1;
            memcpy(net.ssid, ap->ssid, n);
            net.ssid[n] = '\0';
            rb_copy_str(net.bssid, sizeof(net.bssid), ap->hw_address);
            net.signal_dbm = 0; /* NM percent, not dBm — not reported */
            net.frequency_mhz = ap->frequency;
            net.channel = nm_channel_for_freq(ap->frequency);
            net.security = nm_ap_security(ap);
            if (job->cb) job->cb(job->ctx, &net, job->ud);
        }
    }
    for (size_t i = 0; i < path_count; i++) free(paths[i]);
    rb_mutex_lock(&s->lock);
    s->wifi_scan_active = 0;
    rb_mutex_unlock(&s->lock);
    rb_op_mark_done(job->ctx, job->op);
    free(job);
    return NULL;
}

static rb_status_t linux_wifi_scan_start(rb_context_t *ctx,
                                         const rb_wifi_scan_options_t *opts,
                                         rb_wifi_scan_callback_t cb,
                                         void *ud, rb_operation_id_t op) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    (void)opts;
    if (!s->nm_present) return RB_ERROR_UNAVAILABLE;
    if (s->wifi_device_path[0] == '\0') return RB_ERROR_UNAVAILABLE;
    rb_mutex_lock(&s->lock);
    if (s->wifi_scan_active) {
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_ALREADY_IN_PROGRESS;
    }
    s->wifi_scan_active = 1;
    s->wifi_scan_stop = 0;
    s->wifi_scan_op = op;
    rb_mutex_unlock(&s->lock);
    wifi_scan_job_t *job = (wifi_scan_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        rb_mutex_lock(&s->lock);
        s->wifi_scan_active = 0;
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_NO_MEMORY;
    }
    job->ctx = ctx;
    job->cb = cb;
    job->ud = ud;
    job->op = op;
    if (rb_thread_create(&s->wifi_scan_thread, linux_wifi_scan_thread,
                         job) != 0) {
        free(job);
        rb_mutex_lock(&s->lock);
        s->wifi_scan_active = 0;
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_INTERNAL;
    }
    return RB_OK;
}

static rb_status_t linux_wifi_scan_stop(rb_context_t *ctx,
                                        rb_operation_id_t op) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    (void)op;
    rb_mutex_lock(&s->lock);
    if (!s->wifi_scan_active) {
        rb_mutex_unlock(&s->lock);
        return RB_ERROR_NOT_FOUND;
    }
    s->wifi_scan_stop = 1;
    rb_thread_t th = s->wifi_scan_thread;
    rb_mutex_unlock(&s->lock);
    if (!rb_thread_equal(rb_thread_self(), th)) rb_thread_join(th);
    return RB_OK;
}

static void linux_gen_uuid(char *out, size_t len) {
    static unsigned int counter = 0;
    counter++;
    snprintf(out, len, "%08x-%04x-%04x-%04x-%012llx",
             (unsigned int)(rb_now_ms() & 0xffffffffu),
             (unsigned int)(getpid() & 0xffffu),
             (unsigned int)(counter & 0xffffu),
             (unsigned int)((rb_now_ms() >> 16) & 0xffffu),
             (unsigned long long)((rb_now_ms() * 2654435761u) &
                                  0xffffffffffffULL));
}

static rb_status_t linux_wifi_connect(rb_context_t *ctx, const char *ssid,
                                      const char *password,
                                      const rb_wifi_connect_options_t *opts) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!ssid || !*ssid) return RB_ERROR_INVALID_ARGUMENT;
    if (!s->nm_present) return RB_ERROR_UNAVAILABLE;
    if (s->wifi_device_path[0] == '\0') return RB_ERROR_UNAVAILABLE;
    char en[256], dt[256];
    nm_snapshot_t snap;
    if (!nm_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                          sizeof(dt)))
        return rb_dbus_error_to_status(en);
    /* Find the AP by SSID bytes. */
    const nm_ap_snap_t *ap = NULL;
    for (size_t i = 0; i < snap.ap_count; i++) {
        if (snap.aps[i].ssid_len == strlen(ssid) &&
            memcmp(snap.aps[i].ssid, ssid, snap.aps[i].ssid_len) == 0) {
            ap = &snap.aps[i];
            break;
        }
    }
    if (!ap) return RB_ERROR_NOT_FOUND;
    rb_wifi_security_t sec = nm_ap_security(ap);
    if (sec != RB_WIFI_SEC_OPEN && (!password || !*password))
        return RB_ERROR_AUTH_FAILED;
    if (sec == RB_WIFI_SEC_ENTERPRISE)
        return RB_ERROR_NOT_SUPPORTED; /* 802.1X not wired in v0.2 */
    /* Build the connection settings dict a{sa{sv}}. */
    DBusMessage *req = rb_dbus_new_call(RB_NM_SERVICE, RB_NM_ROOT_PATH,
                                        RB_NM_IFACE,
                                        "AddAndActivateConnection");
    if (!req) return RB_ERROR_NO_MEMORY;
    DBusMessageIter it, outer;
    rb_dbus_append_iter(req, &it);
    rb_dbus_open_settings(&it, &outer);
    char uuid_buf[64];
    linux_gen_uuid(uuid_buf, sizeof(uuid_buf));
    rb_dbus_setting_t stn;
    rb_dbus_setting_open(&outer, &stn, "connection");
    rb_dbus_dict_put_string(&stn.dict, "id", ssid);
    rb_dbus_dict_put_string(&stn.dict, "uuid", uuid_buf);
    rb_dbus_dict_put_string(&stn.dict, "type", "802-11-wireless");
    rb_dbus_setting_close(&outer, &stn);
    rb_dbus_setting_open(&outer, &stn, "802-11-wireless");
    rb_dbus_dict_put_byte_array(&stn.dict, "ssid",
                                (const uint8_t *)ssid, strlen(ssid));
    rb_dbus_dict_put_string(&stn.dict, "mode", "infrastructure");
    rb_dbus_setting_close(&outer, &stn);
    if (sec != RB_WIFI_SEC_OPEN) {
        rb_dbus_setting_open(&outer, &stn, "802-11-wireless-security");
        /* wpa-psk covers WPA2 + WPA3 transition per NM docs. */
        rb_dbus_dict_put_string(&stn.dict, "key-mgmt", "wpa-psk");
        rb_dbus_dict_put_string(&stn.dict, "psk", password);
        rb_dbus_setting_close(&outer, &stn);
    }
    rb_dbus_close_settings(&it, &outer);
    rb_dbus_append_object_path(req, s->wifi_device_path);
    rb_dbus_append_object_path(req, ap->path);
    DBusMessage *rep =
        rb_dbus_call(s->bus, req, 15000, en, sizeof(en), dt, sizeof(dt));
    if (!rep) return rb_dbus_error_to_status(en);
    /* Reply: first object path is the active connection (tolerate
     * extra return values from NM variants). */
    char active_path[256] = "";
    DBusMessageIter rit;
    if (dbus_message_iter_init(rep, &rit)) {
        while (dbus_message_iter_get_arg_type(&rit) !=
               DBUS_TYPE_INVALID) {
            if (dbus_message_iter_get_arg_type(&rit) ==
                DBUS_TYPE_OBJECT_PATH) {
                rb_dbus_iter_object_path(&rit, active_path,
                                         sizeof(active_path));
                break;
            }
            dbus_message_iter_next(&rit);
        }
    }
    dbus_message_unref(rep);
    if (active_path[0] == '\0') return RB_ERROR_INTERNAL;
    rb_mutex_lock(&s->lock);
    rb_copy_str(s->active_conn_path, sizeof(s->active_conn_path),
                active_path);
    rb_copy_str(s->connected_ssid, sizeof(s->connected_ssid), ssid);
    rb_mutex_unlock(&s->lock);
    /* Asynchronous per the public contract: activation is requested,
     * and the outcome is visible via rb_wifi_get_status(). The caller
     * polls status; this function does not block on it. (v0.2 does
     * not emit RB_EVENT_WIFI_CHANGED — no signal delivery — so
     * status polling is the way to observe the result.) */
    (void)opts;
    return RB_OK;
}

static rb_status_t linux_wifi_disconnect(rb_context_t *ctx) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!s->nm_present) return RB_ERROR_UNAVAILABLE;
    char path[256] = "";
    rb_mutex_lock(&s->lock);
    rb_copy_str(path, sizeof(path), s->active_conn_path);
    rb_mutex_unlock(&s->lock);
    if (path[0] == '\0') {
        /* Fallback: the device's ActiveConnection property. */
        char en[256], dt[256];
        nm_snapshot_t snap;
        if (nm_snapshot_take(s->bus, &snap, en, sizeof(en), dt,
                             sizeof(dt))) {
            const nm_device_snap_t *w = nm_find_wifi(&snap);
            if (w && w->active_connection[0] &&
                strcmp(w->active_connection, "/") != 0)
                rb_copy_str(path, sizeof(path), w->active_connection);
        }
    }
    if (path[0] == '\0') return RB_OK; /* already disconnected */
    DBusMessage *req = rb_dbus_new_call(RB_NM_SERVICE, RB_NM_ROOT_PATH,
                                        RB_NM_IFACE,
                                        "DeactivateConnection");
    if (!req) return RB_ERROR_NO_MEMORY;
    rb_dbus_append_object_path(req, path);
    char en[256], dt[256];
    DBusMessage *rep =
        rb_dbus_call(s->bus, req, 10000, en, sizeof(en), dt, sizeof(dt));
    if (!rep) return rb_dbus_error_to_status(en);
    dbus_message_unref(rep);
    rb_mutex_lock(&s->lock);
    s->active_conn_path[0] = '\0';
    s->connected_ssid[0] = '\0';
    rb_mutex_unlock(&s->lock);
    return RB_OK;
}

/* TCP-connect latency probe: 3 samples, 2 s per-attempt timeout. */
static int tcp_probe_once(const char *host, const char *port,
                          double *latency_ms) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return 0;
    int ok = 0;
    uint64_t t0 = rb_now_ms();
    for (ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            ok = 1;
        } else {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            struct timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) ==
                        0 &&
                    err == 0)
                    ok = 1;
            }
        }
        close(fd);
    }
    uint64_t t1 = rb_now_ms();
    freeaddrinfo(res);
    if (ok && latency_ms) *latency_ms = (double)(t1 - t0);
    return ok;
}

static rb_status_t linux_wifi_measure_quality(rb_context_t *ctx,
                                              const char *endpoint,
                                              rb_wifi_quality_t *out) {
    linux_state_t *s = linux_get(ctx);
    if (!s) return RB_ERROR_NOT_INITIALIZED;
    if (!endpoint || !*endpoint || !out)
        return RB_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    rb_copy_str(out->endpoint, sizeof(out->endpoint), endpoint);
    /* Parse host[:port], tolerating an http(s):// prefix and path. */
    char hostport[128];
    const char *e = endpoint;
    if (starts_with(e, "http://")) e += 7;
    else if (starts_with(e, "https://")) e += 8;
    rb_copy_str(hostport, sizeof(hostport), e);
    char *slash = strchr(hostport, '/');
    if (slash) *slash = '\0';
    char host[128], port[16];
    rb_copy_str(port, sizeof(port), "80");
    char *colon = strrchr(hostport, ':');
    if (colon && strchr(hostport, ':') == colon) {
        rb_copy_str(host, sizeof(host), hostport);
        char *c2 = strchr(host, ':');
        if (c2) *c2 = '\0';
        rb_copy_str(port, sizeof(port), colon + 1);
    } else {
        rb_copy_str(host, sizeof(host), hostport);
    }
    double samples[3];
    int successes = 0;
    for (int i = 0; i < 3; i++) {
        double lat = 0;
        if (tcp_probe_once(host, port, &lat)) {
            samples[successes++] = lat;
        }
        if (i < 2) rb_sleep_ms(100);
    }
    out->reachable = successes > 0;
    if (successes == 0) {
        out->latency_ms = 0;
        out->jitter_ms = 0;
        out->packet_loss_percent = 100.0;
        return RB_OK;
    }
    double sum = 0;
    for (int i = 0; i < successes; i++) sum += samples[i];
    out->latency_ms = sum / successes;
    double jsum = 0;
    for (int i = 1; i < successes; i++) {
        double d = samples[i] - samples[i - 1];
        jsum += d < 0 ? -d : d;
    }
    out->jitter_ms = successes > 1 ? jsum / (successes - 1) : 0;
    out->packet_loss_percent =
        100.0 * (double)(3 - successes) / 3.0;
    return RB_OK;
}

static rb_status_t linux_required_permissions(rb_operation_t op,
                                              rb_permission_mask_t *mask) {
    if (mask == NULL) return RB_ERROR_INVALID_ARGUMENT;
    switch (op) {
        case RB_OP_BLE_SCAN:
            *mask = RB_PERM_BLUETOOTH_SCAN | RB_PERM_LOCATION;
            break;
        case RB_OP_BLE_CONNECT:
        case RB_OP_BLE_READ:
        case RB_OP_BLE_WRITE:
        case RB_OP_BLE_NOTIFY:
            *mask = RB_PERM_BLUETOOTH_CONNECT;
            break;
        case RB_OP_WIFI_SCAN:
            *mask = RB_PERM_WIFI_SCAN | RB_PERM_LOCATION;
            break;
        case RB_OP_WIFI_CONNECT:
            *mask = RB_PERM_WIFI_CONNECT;
            break;
        default:
            *mask = RB_PERM_NONE;
            break;
    }
    return RB_OK;
}

const rb_backend_ops_t rb_linux_backend_ops = {
    .name = "linux",
    .version = "0.2.0",
    .probe = rb_linux_probe,
    .create = linux_create,
    .destroy = linux_destroy,
    .ble_scan_start = linux_ble_scan_start,
    .ble_scan_stop = linux_ble_scan_stop,
    .ble_connect = linux_ble_connect,
    .ble_disconnect = linux_ble_disconnect,
    .ble_discover = linux_ble_discover,
    .ble_read = linux_ble_read,
    .ble_write = linux_ble_write,
    .ble_subscribe = linux_ble_subscribe,
    .ble_unsubscribe = linux_ble_unsubscribe,
    .ble_get_rssi = linux_ble_get_rssi,
    .ble_set_reconnect_policy = linux_ble_set_reconnect_policy,
    .wifi_get_status = linux_wifi_get_status,
    .wifi_scan_start = linux_wifi_scan_start,
    .wifi_scan_stop = linux_wifi_scan_stop,
    .wifi_connect = linux_wifi_connect,
    .wifi_disconnect = linux_wifi_disconnect,
    .wifi_measure_quality = linux_wifi_measure_quality,
    .required_permissions = linux_required_permissions,
    .mock_trigger_disconnect = NULL,
    .mock_trigger_wifi_change = NULL,
    .diagnostics = linux_diagnostics,
};

#endif /* RB_HAVE_DBUS */

#if !defined(RB_HAVE_DBUS) || defined(_WIN32)
/* Fallback ops table: every operation reports unavailable. */
static rb_status_t linux_fb_create(rb_context_t *ctx) {
    rb_capabilities_t caps;
    rb_linux_probe(&caps);
    rb_logf(ctx, RB_LOG_WARN, "linux", "%s", caps.detail);
    ctx->caps = caps;
    return RB_ERROR_UNAVAILABLE;
}

static void linux_fb_destroy(rb_context_t *ctx) {
    ctx->backend_state = NULL;
}

static rb_status_t linux_fb_ble_scan_start(rb_context_t *ctx, const rb_ble_scan_options_t *o, rb_ble_scan_callback_t c, void *u, rb_operation_id_t op) { (void)ctx; (void)o; (void)c; (void)u; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_scan_stop(rb_context_t *ctx, rb_operation_id_t op) { (void)ctx; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_connect(rb_context_t *ctx, const rb_device_id_t *d, const rb_ble_connect_options_t *o, rb_ble_connection_callback_t c, void *u, rb_operation_id_t op) { (void)ctx; (void)d; (void)o; (void)c; (void)u; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_disconnect(rb_context_t *ctx, const rb_device_id_t *d) { (void)ctx; (void)d; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_discover(rb_context_t *ctx, const rb_device_id_t *d, rb_ble_service_t **o, size_t *n) { (void)ctx; (void)d; (void)o; (void)n; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_read(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c, uint8_t *v, size_t *n) { (void)ctx; (void)d; (void)s; (void)c; (void)v; (void)n; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_write(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c, const uint8_t *v, size_t n, int w) { (void)ctx; (void)d; (void)s; (void)c; (void)v; (void)n; (void)w; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_subscribe(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c, rb_ble_notify_callback_t cb, void *u) { (void)ctx; (void)d; (void)s; (void)c; (void)cb; (void)u; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_unsubscribe(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c) { (void)ctx; (void)d; (void)s; (void)c; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_get_rssi(rb_context_t *ctx, const rb_device_id_t *d, int8_t *r) { (void)ctx; (void)d; (void)r; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_ble_set_reconnect_policy(rb_context_t *ctx, const rb_reconnect_policy_t *p) { (void)ctx; (void)p; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_wifi_get_status(rb_context_t *ctx, rb_wifi_status_t *o) { (void)ctx; (void)o; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_wifi_scan_start(rb_context_t *ctx, const rb_wifi_scan_options_t *o, rb_wifi_scan_callback_t c, void *u, rb_operation_id_t op) { (void)ctx; (void)o; (void)c; (void)u; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_wifi_scan_stop(rb_context_t *ctx, rb_operation_id_t op) { (void)ctx; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_wifi_connect(rb_context_t *ctx, const char *s, const char *p, const rb_wifi_connect_options_t *o) { (void)ctx; (void)s; (void)p; (void)o; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_wifi_disconnect(rb_context_t *ctx) { (void)ctx; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_fb_wifi_measure_quality(rb_context_t *ctx, const char *e, rb_wifi_quality_t *o) { (void)ctx; (void)e; (void)o; return RB_ERROR_UNAVAILABLE; }

static rb_status_t linux_fb_required_permissions(rb_operation_t op, rb_permission_mask_t *mask) {
    if (mask == NULL) return RB_ERROR_INVALID_ARGUMENT;
    switch (op) {
        case RB_OP_BLE_SCAN:
            *mask = RB_PERM_BLUETOOTH_SCAN | RB_PERM_LOCATION;
            break;
        case RB_OP_BLE_CONNECT:
        case RB_OP_BLE_READ:
        case RB_OP_BLE_WRITE:
        case RB_OP_BLE_NOTIFY:
            *mask = RB_PERM_BLUETOOTH_CONNECT;
            break;
        case RB_OP_WIFI_SCAN:
            *mask = RB_PERM_WIFI_SCAN | RB_PERM_LOCATION;
            break;
        case RB_OP_WIFI_CONNECT:
            *mask = RB_PERM_WIFI_CONNECT;
            break;
        default:
            *mask = RB_PERM_NONE;
            break;
    }
    return RB_OK;
}

const rb_backend_ops_t rb_linux_backend_ops = {
    .name = "linux",
    .version = "0.2.0",
    .probe = rb_linux_probe,
    .create = linux_fb_create,
    .destroy = linux_fb_destroy,
    .ble_scan_start = linux_fb_ble_scan_start,
    .ble_scan_stop = linux_fb_ble_scan_stop,
    .ble_connect = linux_fb_ble_connect,
    .ble_disconnect = linux_fb_ble_disconnect,
    .ble_discover = linux_fb_ble_discover,
    .ble_read = linux_fb_ble_read,
    .ble_write = linux_fb_ble_write,
    .ble_subscribe = linux_fb_ble_subscribe,
    .ble_unsubscribe = linux_fb_ble_unsubscribe,
    .ble_get_rssi = linux_fb_ble_get_rssi,
    .ble_set_reconnect_policy = linux_fb_ble_set_reconnect_policy,
    .wifi_get_status = linux_fb_wifi_get_status,
    .wifi_scan_start = linux_fb_wifi_scan_start,
    .wifi_scan_stop = linux_fb_wifi_scan_stop,
    .wifi_connect = linux_fb_wifi_connect,
    .wifi_disconnect = linux_fb_wifi_disconnect,
    .wifi_measure_quality = linux_fb_wifi_measure_quality,
    .required_permissions = linux_fb_required_permissions,
    .mock_trigger_disconnect = NULL,
    .mock_trigger_wifi_change = NULL,
    .diagnostics = NULL,
};
#endif /* no DBUS or Windows */
