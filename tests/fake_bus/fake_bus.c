/*
 * Fake BlueZ + NetworkManager service for RadioBridge's Linux backend
 * test (test_linux_bus). This is a TEST TOOL, not part of the library:
 * it owns org.bluez and org.freedesktop.NetworkManager on a private
 * bus (address from RB_DBUS_ADDRESS) and exposes the real interfaces
 * with canned devices/networks, so the backend is verified over the
 * real D-Bus protocol without any hardware.
 *
 * Canned inventory:
 *  - Adapter /org/bluez/hci0, address 00:11:22:33:44:55, powered.
 *  - Device AA:BB:CC:00:00:01 "RB Fake Heart", RSSI -52, service 180D,
 *    char 2A37 [read, notify], value 0x48 0x00, desc 2902.
 *  - Device AA:BB:CC:00:00:02 "RB Fake Thermo", RSSI -68, service
 *    1809, char 2A1C [read], value 0x15 0x00, desc 2902.
 *  - NM device (Wi-Fi), AP "RB-FakeNet" (open, 2412 MHz) and
 *    "RB-FakeSecure" (WPA2 via RsnFlags, 5180 MHz), IP4Config
 *    192.168.7.42 / gateway 192.168.7.1, NM version "1.40.0-rb-fake".
 *
 * Notification delivery is lazy, not threaded: after StartNotify, a
 * Properties.Get for Value returns the flipped value (0x49 0x00)
 * once 150 ms have elapsed. The backend polls Value, so it observes
 * the change exactly as it would from a real adapter's notify.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <dbus/dbus.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------------ */
/* Canned state                                                        */
/* ------------------------------------------------------------------ */

#define DEV1_PATH "/org/bluez/hci0/dev_AA_BB_CC_00_00_01"
#define DEV2_PATH "/org/bluez/hci0/dev_AA_BB_CC_00_00_02"
#define SVC1_PATH DEV1_PATH "/service0001"
#define CHR1_PATH SVC1_PATH "/char0001"
#define DESC1_PATH CHR1_PATH "/desc0001"
#define SVC2_PATH DEV2_PATH "/service0002"
#define CHR2_PATH SVC2_PATH "/char0002"
#define DESC2_PATH CHR2_PATH "/desc0002"

#define NM_ROOT "/org/freedesktop/NetworkManager"
#define NM_DEV NM_ROOT "/Devices/1"
#define NM_AP1 NM_ROOT "/AccessPoints/1"
#define NM_AP2 NM_ROOT "/AccessPoints/2"
#define NM_IP4 NM_ROOT "/IP4Config/1"
#define NM_AC NM_ROOT "/ActiveConnection/1"

static int g_discovering = 0;
static int g_dev1_connected = 0, g_dev1_resolved = 0;
static int g_dev2_connected = 0, g_dev2_resolved = 0;
static int g_chr1_notifying = 0;
static uint64_t g_chr1_notify_start = 0;
static uint8_t g_chr1_written[512];
static size_t g_chr1_written_len = 0;

static int g_nm_state = 30; /* DISCONNECTED */
static const char *g_nm_active_ap = "/";
static const char *g_nm_ip4config = "/";
static const char *g_nm_active_conn = "/";

static const uint8_t *chr1_value(size_t *len) {
    static uint8_t v0[2] = {0x48, 0x00};
    static uint8_t v1[2] = {0x49, 0x00};
    if (g_chr1_notifying && now_ms() - g_chr1_notify_start >= 150) {
        *len = 2;
        return v1;
    }
    *len = 2;
    return v0;
}

/* ------------------------------------------------------------------ */
/* Reply-building helpers                                              */
/* ------------------------------------------------------------------ */

static void dict_entry_open(DBusMessageIter *dict, const char *key,
                            const char *sig, DBusMessageIter *entry,
                            DBusMessageIter *var) {
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL,
                                     entry);
    dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(entry, DBUS_TYPE_VARIANT, sig, var);
}

static void dict_entry_close(DBusMessageIter *dict, DBusMessageIter *entry,
                             DBusMessageIter *var) {
    dbus_message_iter_close_container(entry, var);
    dbus_message_iter_close_container(dict, entry);
}

static void prop_str(DBusMessageIter *dict, const char *key,
                     const char *val) {
    DBusMessageIter e, v;
    const char *s = val ? val : "";
    dict_entry_open(dict, key, "s", &e, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
    dict_entry_close(dict, &e, &v);
}

static void prop_bool(DBusMessageIter *dict, const char *key, int val) {
    DBusMessageIter e, v;
    dbus_bool_t b = val ? TRUE : FALSE;
    dict_entry_open(dict, key, "b", &e, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
    dict_entry_close(dict, &e, &v);
}

static void prop_i16(DBusMessageIter *dict, const char *key, int16_t val) {
    DBusMessageIter e, v;
    dict_entry_open(dict, key, "n", &e, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT16, &val);
    dict_entry_close(dict, &e, &v);
}

static void prop_u32(DBusMessageIter *dict, const char *key,
                     uint32_t val) {
    DBusMessageIter e, v;
    dict_entry_open(dict, key, "u", &e, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &val);
    dict_entry_close(dict, &e, &v);
}

static void prop_objpath(DBusMessageIter *dict, const char *key,
                         const char *val) {
    DBusMessageIter e, v;
    const char *s = val ? val : "/";
    dict_entry_open(dict, key, "o", &e, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &s);
    dict_entry_close(dict, &e, &v);
}

static void prop_bytes(DBusMessageIter *dict, const char *key,
                       const uint8_t *bytes, size_t len) {
    DBusMessageIter e, v, arr;
    dict_entry_open(dict, key, "ay", &e, &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "y", &arr);
    if (len > 0)
        dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE,
                                             &bytes, (int)len);
    dbus_message_iter_close_container(&v, &arr);
    dict_entry_close(dict, &e, &v);
}

static void prop_strv(DBusMessageIter *dict, const char *key,
                      const char **vals, size_t count) {
    DBusMessageIter e, v, arr;
    dict_entry_open(dict, key, "as", &e, &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &arr);
    for (size_t i = 0; i < count; i++) {
        const char *s = vals[i];
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s);
    }
    dbus_message_iter_close_container(&v, &arr);
    dict_entry_close(dict, &e, &v);
}

static void prop_mfr(DBusMessageIter *dict, uint16_t id,
                     const uint8_t *bytes, size_t len) {
    DBusMessageIter e, v, arr, mentry, barr;
    dict_entry_open(dict, "ManufacturerData", "a{qay}", &e, &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "{qay}", &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL,
                                     &mentry);
    dbus_message_iter_append_basic(&mentry, DBUS_TYPE_UINT16, &id);
    /* NOTE: a{qay} values are plain byte arrays, NOT variants — an
     * earlier version opened a variant here and libdbus aborted the
     * service on the type mismatch. */
    dbus_message_iter_open_container(&mentry, DBUS_TYPE_ARRAY, "y",
                                     &barr);
    if (len > 0)
        dbus_message_iter_append_fixed_array(&barr, DBUS_TYPE_BYTE,
                                             &bytes, (int)len);
    dbus_message_iter_close_container(&mentry, &barr);
    dbus_message_iter_close_container(&arr, &mentry);
    dbus_message_iter_close_container(&v, &arr);
    dict_entry_close(dict, &e, &v);
}

/* ObjectManager frame helpers: caller keeps all iters on the stack. */
static void mo_obj_begin(DBusMessageIter *top, DBusMessageIter *entry,
                         DBusMessageIter *ifaces, const char *path) {
    dbus_message_iter_open_container(top, DBUS_TYPE_DICT_ENTRY, NULL,
                                     entry);
    dbus_message_iter_append_basic(entry, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_open_container(entry, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                     ifaces);
}

static void mo_obj_end(DBusMessageIter *top, DBusMessageIter *entry,
                       DBusMessageIter *ifaces) {
    dbus_message_iter_close_container(entry, ifaces);
    dbus_message_iter_close_container(top, entry);
}

static void mo_iface_begin(DBusMessageIter *ifaces, DBusMessageIter *entry,
                           DBusMessageIter *props, const char *iface) {
    dbus_message_iter_open_container(ifaces, DBUS_TYPE_DICT_ENTRY, NULL,
                                     entry);
    dbus_message_iter_append_basic(entry, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(entry, DBUS_TYPE_ARRAY, "{sv}",
                                     props);
}

static void mo_iface_end(DBusMessageIter *ifaces, DBusMessageIter *entry,
                         DBusMessageIter *props) {
    dbus_message_iter_close_container(entry, props);
    dbus_message_iter_close_container(ifaces, entry);
}

/* ------------------------------------------------------------------ */
/* GetManagedObjects builders                                          */
/* ------------------------------------------------------------------ */

static void build_bluez_mo(DBusMessage *reply) {
    DBusMessageIter it, top;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
                                     &top);
    DBusMessageIter oe, ifs, ie, pr;

    /* Adapter. */
    mo_obj_begin(&top, &oe, &ifs, "/org/bluez/hci0");
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.Adapter1");
    prop_str(&pr, "Address", "00:11:22:33:44:55");
    prop_bool(&pr, "Powered", 1);
    prop_bool(&pr, "Discovering", g_discovering);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    /* Device 1 + GATT. */
    static const uint8_t mfr1[2] = {0x01, 0x02};
    static const char *uuids1[1] = {
        "0000180d-0000-1000-8000-00805f9b34fb"};
    static const char *flags1[2] = {"read", "notify"};
    mo_obj_begin(&top, &oe, &ifs, DEV1_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.Device1");
    prop_str(&pr, "Address", "AA:BB:CC:00:00:01");
    prop_str(&pr, "Name", "RB Fake Heart");
    prop_str(&pr, "Alias", "RB Fake Heart");
    prop_i16(&pr, "RSSI", -52);
    prop_i16(&pr, "TxPower", -10);
    prop_mfr(&pr, 0x004c, mfr1, sizeof(mfr1));
    prop_strv(&pr, "UUIDs", uuids1, 1);
    prop_bool(&pr, "Connected", g_dev1_connected);
    prop_bool(&pr, "Paired", 0);
    prop_bool(&pr, "ServicesResolved", g_dev1_resolved);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, SVC1_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.GattService1");
    prop_str(&pr, "UUID", "0000180d-0000-1000-8000-00805f9b34fb");
    prop_bool(&pr, "Primary", 1);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, CHR1_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.GattCharacteristic1");
    prop_str(&pr, "UUID", "00002a37-0000-1000-8000-00805f9b34fb");
    prop_strv(&pr, "Flags", flags1, 2);
    size_t vlen = 0;
    prop_bytes(&pr, "Value", chr1_value(&vlen), vlen);
    prop_bool(&pr, "Notifying", g_chr1_notifying);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, DESC1_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.GattDescriptor1");
    prop_str(&pr, "UUID", "00002902-0000-1000-8000-00805f9b34fb");
    static const uint8_t cccd[2] = {0x00, 0x00};
    prop_bytes(&pr, "Value", cccd, sizeof(cccd));
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    /* Device 2 + GATT. */
    static const char *uuids2[1] = {
        "00001809-0000-1000-8000-00805f9b34fb"};
    static const char *flags2[1] = {"read"};
    mo_obj_begin(&top, &oe, &ifs, DEV2_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.Device1");
    prop_str(&pr, "Address", "AA:BB:CC:00:00:02");
    prop_str(&pr, "Name", "RB Fake Thermo");
    prop_str(&pr, "Alias", "RB Fake Thermo");
    prop_i16(&pr, "RSSI", -68);
    prop_strv(&pr, "UUIDs", uuids2, 1);
    prop_bool(&pr, "Connected", g_dev2_connected);
    prop_bool(&pr, "Paired", 0);
    prop_bool(&pr, "ServicesResolved", g_dev2_resolved);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, SVC2_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.GattService1");
    prop_str(&pr, "UUID", "00001809-0000-1000-8000-00805f9b34fb");
    prop_bool(&pr, "Primary", 1);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, CHR2_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.GattCharacteristic1");
    prop_str(&pr, "UUID", "00002a1c-0000-1000-8000-00805f9b34fb");
    prop_strv(&pr, "Flags", flags2, 1);
    static const uint8_t temp[2] = {0x15, 0x00};
    prop_bytes(&pr, "Value", temp, sizeof(temp));
    prop_bool(&pr, "Notifying", 0);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, DESC2_PATH);
    mo_iface_begin(&ifs, &ie, &pr, "org.bluez.GattDescriptor1");
    prop_str(&pr, "UUID", "00002902-0000-1000-8000-00805f9b34fb");
    prop_bytes(&pr, "Value", cccd, sizeof(cccd));
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    dbus_message_iter_close_container(&it, &top);
}

static void build_nm_mo(DBusMessage *reply) {
    DBusMessageIter it, top;
    dbus_message_iter_init_append(reply, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
                                     &top);
    DBusMessageIter oe, ifs, ie, pr;

    mo_obj_begin(&top, &oe, &ifs, NM_ROOT);
    mo_iface_begin(&ifs, &ie, &pr, "org.freedesktop.NetworkManager");
    prop_str(&pr, "Version", "1.40.0-rb-fake");
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, NM_DEV);
    mo_iface_begin(&ifs, &ie, &pr, "org.freedesktop.NetworkManager.Device");
    prop_u32(&pr, "DeviceType", 2);
    prop_u32(&pr, "State", (uint32_t)g_nm_state);
    prop_objpath(&pr, "ActiveAccessPoint", g_nm_active_ap);
    prop_objpath(&pr, "Ip4Config", g_nm_ip4config);
    prop_objpath(&pr, "ActiveConnection", g_nm_active_conn);
    mo_iface_end(&ifs, &ie, &pr);
    mo_iface_begin(&ifs, &ie, &pr,
                   "org.freedesktop.NetworkManager.Device.Wireless");
    /* No properties needed from the wireless iface in v0.2. */
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    static const uint8_t ssid1[10] = "RB-FakeNet";
    static const uint8_t ssid2[13] = "RB-FakeSecure";
    mo_obj_begin(&top, &oe, &ifs, NM_AP1);
    mo_iface_begin(&ifs, &ie, &pr,
                   "org.freedesktop.NetworkManager.AccessPoint");
    prop_bytes(&pr, "Ssid", ssid1, 10);
    prop_str(&pr, "HwAddress", "11:22:33:44:55:66");
    prop_u32(&pr, "Strength", 80);
    prop_u32(&pr, "Flags", 0);
    prop_u32(&pr, "WpaFlags", 0);
    prop_u32(&pr, "RsnFlags", 0);
    prop_u32(&pr, "Frequency", 2412);
    prop_u32(&pr, "Mode", 2);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, NM_AP2);
    mo_iface_begin(&ifs, &ie, &pr,
                   "org.freedesktop.NetworkManager.AccessPoint");
    prop_bytes(&pr, "Ssid", ssid2, 13);
    prop_str(&pr, "HwAddress", "11:22:33:44:55:77");
    prop_u32(&pr, "Strength", 65);
    prop_u32(&pr, "Flags", 0);
    prop_u32(&pr, "WpaFlags", 0);
    prop_u32(&pr, "RsnFlags", 1);
    prop_u32(&pr, "Frequency", 5180);
    prop_u32(&pr, "Mode", 2);
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    mo_obj_begin(&top, &oe, &ifs, NM_IP4);
    mo_iface_begin(&ifs, &ie, &pr,
                   "org.freedesktop.NetworkManager.IP4Config");
    prop_str(&pr, "Gateway", "192.168.7.1");
    /* AddressData: aa{sv} — an array of address dictionaries. */
    {
        DBusMessageIter e, v, arr, dict;
        dict_entry_open(&pr, "AddressData", "aa{sv}", &e, &v);
        dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "a{sv}",
                                         &arr);
        dbus_message_iter_open_container(&arr, DBUS_TYPE_ARRAY, "{sv}",
                                         &dict);
        prop_str(&dict, "address", "192.168.7.42");
        prop_u32(&dict, "prefix", 24);
        prop_str(&dict, "gateway", "192.168.7.1");
        dbus_message_iter_close_container(&arr, &dict);
        dbus_message_iter_close_container(&v, &arr);
        dict_entry_close(&pr, &e, &v);
    }
    mo_iface_end(&ifs, &ie, &pr);
    mo_obj_end(&top, &oe, &ifs);

    dbus_message_iter_close_container(&it, &top);
}

/* ------------------------------------------------------------------ */
/* Properties.Get                                                      */
/* ------------------------------------------------------------------ */

/* Append one property value as a variant to the reply. Returns 1 when
 * the property is known, 0 otherwise. */
static int append_prop_value(DBusMessage *reply, const char *path,
                             const char *iface, const char *prop) {
    DBusMessageIter it, var;
    dbus_message_iter_init_append(reply, &it);
    int known = 1;

    /* Helper macros: open variant with signature, append, close. */
#define VARIANT_BEGIN(sig) \
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var)
#define VARIANT_END() dbus_message_iter_close_container(&it, &var)

    if (strcmp(path, "/org/bluez/hci0") == 0 &&
        strcmp(iface, "org.bluez.Adapter1") == 0) {
        if (strcmp(prop, "Address") == 0) {
            const char *s = "00:11:22:33:44:55";
            VARIANT_BEGIN("s");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &s);
            VARIANT_END();
        } else if (strcmp(prop, "Powered") == 0) {
            dbus_bool_t b = TRUE;
            VARIANT_BEGIN("b");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            VARIANT_END();
        } else if (strcmp(prop, "Discovering") == 0) {
            dbus_bool_t b = g_discovering ? TRUE : FALSE;
            VARIANT_BEGIN("b");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            VARIANT_END();
        } else {
            known = 0;
        }
    } else if ((strcmp(path, DEV1_PATH) == 0 ||
                strcmp(path, DEV2_PATH) == 0) &&
               strcmp(iface, "org.bluez.Device1") == 0) {
        int is_dev1 = strcmp(path, DEV1_PATH) == 0;
        if (strcmp(prop, "Connected") == 0) {
            dbus_bool_t b =
                (is_dev1 ? g_dev1_connected : g_dev2_connected) ? TRUE
                                                                 : FALSE;
            VARIANT_BEGIN("b");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            VARIANT_END();
        } else if (strcmp(prop, "ServicesResolved") == 0) {
            dbus_bool_t b =
                (is_dev1 ? g_dev1_resolved : g_dev2_resolved) ? TRUE
                                                               : FALSE;
            VARIANT_BEGIN("b");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            VARIANT_END();
        } else if (strcmp(prop, "RSSI") == 0) {
            int16_t r = is_dev1 ? -52 : -68;
            VARIANT_BEGIN("n");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_INT16, &r);
            VARIANT_END();
        } else if (strcmp(prop, "Address") == 0) {
            const char *s =
                is_dev1 ? "AA:BB:CC:00:00:01" : "AA:BB:CC:00:00:02";
            VARIANT_BEGIN("s");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &s);
            VARIANT_END();
        } else {
            known = 0;
        }
    } else if ((strcmp(path, CHR1_PATH) == 0 ||
                strcmp(path, CHR2_PATH) == 0) &&
               strcmp(iface, "org.bluez.GattCharacteristic1") == 0) {
        if (strcmp(prop, "Value") == 0) {
            size_t len = 0;
            const uint8_t *val;
            static const uint8_t temp[2] = {0x15, 0x00};
            if (strcmp(path, CHR1_PATH) == 0) {
                val = chr1_value(&len);
            } else {
                val = temp;
                len = 2;
            }
            VARIANT_BEGIN("ay");
            DBusMessageIter arr;
            dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "y",
                                             &arr);
            if (len > 0)
                dbus_message_iter_append_fixed_array(
                    &arr, DBUS_TYPE_BYTE, &val, (int)len);
            dbus_message_iter_close_container(&var, &arr);
            VARIANT_END();
        } else if (strcmp(prop, "Notifying") == 0) {
            dbus_bool_t b =
                (strcmp(path, CHR1_PATH) == 0 && g_chr1_notifying)
                    ? TRUE
                    : FALSE;
            VARIANT_BEGIN("b");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            VARIANT_END();
        } else {
            known = 0;
        }
    } else if (strcmp(path, NM_ROOT) == 0 &&
               strcmp(iface, "org.freedesktop.NetworkManager") == 0) {
        if (strcmp(prop, "Version") == 0) {
            const char *s = "1.40.0-rb-fake";
            VARIANT_BEGIN("s");
            dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &s);
            VARIANT_END();
        } else {
            known = 0;
        }
    } else {
        known = 0;
    }
#undef VARIANT_BEGIN
#undef VARIANT_END
    return known;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */

static DBusMessage *error_reply(DBusMessage *msg, const char *name,
                                const char *text) {
    return dbus_message_new_error(msg, name, text);
}

static DBusMessage *empty_reply(DBusMessage *msg) {
    return dbus_message_new_method_return(msg);
}

/* Parse AddAndActivateConnection settings; on success activate the
 * matching AP and return 1 (active path in out_path). On validation
 * failure return 0. */
static int nm_activate(DBusMessage *msg, char *out_path, size_t out_len) {
    DBusMessageIter it, settings;
    if (!dbus_message_iter_init(msg, &it)) return 0;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return 0;
    dbus_message_iter_recurse(&it, &settings);

    char id[64] = "";
    char type[64] = "";
    uint8_t ssid[64];
    size_t ssid_len = 0;
    char key_mgmt[32] = "";
    char psk[128] = "";
    int have_security = 0;

    while (dbus_message_iter_get_arg_type(&settings) ==
           DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&settings, &entry);
        const char *name = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &name);
        if (!dbus_message_iter_next(&entry)) {
            dbus_message_iter_next(&settings);
            continue;
        }
        /* Section value: plain a{sv} array per the NM spec. Tolerate
         * a variant-wrapped array too (an earlier backend version
         * sent that form by mistake). */
        DBusMessageIter dict = entry;
        if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_VARIANT)
            dbus_message_iter_recurse(&dict, &dict);
        if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_ARRAY) {
            dbus_message_iter_next(&settings);
            continue;
        }
        dbus_message_iter_recurse(&dict, &dict);
        while (dbus_message_iter_get_arg_type(&dict) ==
               DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter de;
            dbus_message_iter_recurse(&dict, &de);
            const char *key = NULL;
            if (dbus_message_iter_get_arg_type(&de) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&de, &key);
            if (key && dbus_message_iter_next(&de) &&
                dbus_message_iter_get_arg_type(&de) ==
                    DBUS_TYPE_VARIANT) {
                DBusMessageIter inner;
                dbus_message_iter_recurse(&de, &inner);
                if (name && strcmp(name, "connection") == 0) {
                    if (strcmp(key, "id") == 0 &&
                        dbus_message_iter_get_arg_type(&inner) ==
                            DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&inner, &s);
                        if (s) snprintf(id, sizeof(id), "%s", s);
                    } else if (strcmp(key, "type") == 0 &&
                               dbus_message_iter_get_arg_type(&inner) ==
                                   DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&inner, &s);
                        if (s) snprintf(type, sizeof(type), "%s", s);
                    }
                } else if (name &&
                           strcmp(name, "802-11-wireless") == 0) {
                    if (strcmp(key, "ssid") == 0 &&
                        dbus_message_iter_get_arg_type(&inner) ==
                            DBUS_TYPE_ARRAY) {
                        DBusMessageIter arr;
                        dbus_message_iter_recurse(&inner, &arr);
                        void *raw = NULL;
                        int n = 0;
                        dbus_message_iter_get_fixed_array(&arr, &raw,
                                                          &n);
                        if (n > 0 && raw) {
                            ssid_len = (size_t)n < sizeof(ssid)
                                           ? (size_t)n
                                           : sizeof(ssid);
                            memcpy(ssid, raw, ssid_len);
                        }
                    }
                } else if (name && strcmp(name, "802-11-wireless-security") ==
                                      0) {
                    have_security = 1;
                    if (strcmp(key, "key-mgmt") == 0 &&
                        dbus_message_iter_get_arg_type(&inner) ==
                            DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&inner, &s);
                        if (s)
                            snprintf(key_mgmt, sizeof(key_mgmt), "%s",
                                     s);
                    } else if (strcmp(key, "psk") == 0 &&
                               dbus_message_iter_get_arg_type(&inner) ==
                                   DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&inner, &s);
                        if (s) snprintf(psk, sizeof(psk), "%s", s);
                    }
                }
            }
            dbus_message_iter_next(&dict);
        }
        dbus_message_iter_next(&settings);
    }

    /* Validate. */
    if (strcmp(type, "802-11-wireless") != 0) return 0;
    if (id[0] == '\0' || ssid_len == 0) return 0;
    static const uint8_t ssid1[10] = "RB-FakeNet";
    static const uint8_t ssid2[13] = "RB-FakeSecure";
    const char *ap_path = NULL;
    int secured = 0;
    if (ssid_len == 10 && memcmp(ssid, ssid1, 10) == 0) {
        ap_path = NM_AP1;
        secured = 0;
    } else if (ssid_len == 13 && memcmp(ssid, ssid2, 13) == 0) {
        ap_path = NM_AP2;
        secured = 1;
    } else {
        return 0; /* unknown SSID */
    }
    if (secured) {
        if (!have_security || strcmp(key_mgmt, "wpa-psk") != 0 ||
            psk[0] == '\0')
            return 0;
    }
    g_nm_state = 100;
    g_nm_active_ap = ap_path;
    g_nm_ip4config = NM_IP4;
    g_nm_active_conn = NM_AC;
    snprintf(out_path, out_len, "%s", NM_AC);
    return 1;
}

static DBusHandlerResult filter_fn(DBusConnection *conn, DBusMessage *msg,
                                   void *ud) {
    (void)ud;
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *dest = dbus_message_get_destination(msg);
    const char *path = dbus_message_get_path(msg);
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!dest || !path || !iface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = NULL;

    if (strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0 &&
        strcmp(member, "GetManagedObjects") == 0) {
        reply = dbus_message_new_method_return(msg);
        if (strcmp(dest, "org.bluez") == 0)
            build_bluez_mo(reply);
        else if (strcmp(dest, "org.freedesktop.NetworkManager") == 0)
            build_nm_mo(reply);
        else
            reply = error_reply(msg, "org.freedesktop.DBus.Error.UnknownMethod",
                                "unknown destination");
    } else if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0 &&
               strcmp(member, "Get") == 0) {
        DBusMessageIter it;
        const char *riface = NULL, *prop = NULL;
        if (dbus_message_iter_init(msg, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&it, &riface);
            if (dbus_message_iter_next(&it) &&
                dbus_message_iter_get_arg_type(&it) ==
                    DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&it, &prop);
        }
        if (riface && prop) {
            reply = dbus_message_new_method_return(msg);
            if (!append_prop_value(reply, path, riface, prop)) {
                dbus_message_unref(reply);
                reply = error_reply(
                    msg, "org.freedesktop.DBus.Error.InvalidArgs",
                    "unknown property");
            }
        } else {
            reply = error_reply(msg, "org.freedesktop.DBus.Error.InvalidArgs",
                                "bad Get arguments");
        }
    } else if (strcmp(iface, "org.bluez.Adapter1") == 0) {
        if (strcmp(member, "StartDiscovery") == 0) {
            g_discovering = 1;
            reply = empty_reply(msg);
        } else if (strcmp(member, "StopDiscovery") == 0) {
            g_discovering = 0;
            reply = empty_reply(msg);
        }
    } else if (strcmp(iface, "org.bluez.Device1") == 0) {
        if (strcmp(member, "Connect") == 0) {
            if (strcmp(path, DEV1_PATH) == 0) {
                g_dev1_connected = 1;
                g_dev1_resolved = 1;
            } else if (strcmp(path, DEV2_PATH) == 0) {
                g_dev2_connected = 1;
                g_dev2_resolved = 1;
            }
            reply = empty_reply(msg);
        } else if (strcmp(member, "Disconnect") == 0) {
            if (strcmp(path, DEV1_PATH) == 0) {
                g_dev1_connected = 0;
                g_dev1_resolved = 0;
            } else if (strcmp(path, DEV2_PATH) == 0) {
                g_dev2_connected = 0;
                g_dev2_resolved = 0;
            }
            reply = empty_reply(msg);
        } else if (strcmp(member, "Pair") == 0) {
            reply = empty_reply(msg);
        }
    } else if (strcmp(iface, "org.bluez.GattCharacteristic1") == 0) {
        if (strcmp(member, "ReadValue") == 0) {
            reply = dbus_message_new_method_return(msg);
            DBusMessageIter it, arr;
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y",
                                             &arr);
            size_t len = 0;
            const uint8_t *val = NULL;
            static const uint8_t temp[2] = {0x15, 0x00};
            if (strcmp(path, CHR1_PATH) == 0) {
                val = chr1_value(&len);
            } else {
                val = temp;
                len = 2;
            }
            if (len > 0)
                dbus_message_iter_append_fixed_array(
                    &arr, DBUS_TYPE_BYTE, &val, (int)len);
            dbus_message_iter_close_container(&it, &arr);
        } else if (strcmp(member, "WriteValue") == 0) {
            DBusMessageIter it;
            if (dbus_message_iter_init(msg, &it) &&
                dbus_message_iter_get_arg_type(&it) ==
                    DBUS_TYPE_ARRAY) {
                DBusMessageIter arr;
                dbus_message_iter_recurse(&it, &arr);
                void *raw = NULL;
                int n = 0;
                dbus_message_iter_get_fixed_array(&arr, &raw, &n);
                if (n > 0 && raw) {
                    g_chr1_written_len =
                        (size_t)n < sizeof(g_chr1_written)
                            ? (size_t)n
                            : sizeof(g_chr1_written);
                    memcpy(g_chr1_written, raw, g_chr1_written_len);
                }
            }
            reply = empty_reply(msg);
        } else if (strcmp(member, "StartNotify") == 0) {
            if (strcmp(path, CHR1_PATH) == 0) {
                g_chr1_notifying = 1;
                g_chr1_notify_start = now_ms();
            }
            reply = empty_reply(msg);
        } else if (strcmp(member, "StopNotify") == 0) {
            if (strcmp(path, CHR1_PATH) == 0) g_chr1_notifying = 0;
            reply = empty_reply(msg);
        }
    } else if (strcmp(iface, "org.freedesktop.NetworkManager") == 0) {
        if (strcmp(member, "GetDevices") == 0) {
            reply = dbus_message_new_method_return(msg);
            DBusMessageIter it, arr;
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o",
                                             &arr);
            const char *d = NM_DEV;
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_OBJECT_PATH,
                                           &d);
            dbus_message_iter_close_container(&it, &arr);
        } else if (strcmp(member, "AddAndActivateConnection") == 0) {
            char active[256];
            if (nm_activate(msg, active, sizeof(active))) {
                reply = dbus_message_new_method_return(msg);
                const char *a = active;
                dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH,
                                         &a, DBUS_TYPE_INVALID);
            } else {
                reply = error_reply(
                    msg, "org.freedesktop.DBus.Error.InvalidArgs",
                    "connection settings rejected");
            }
        } else if (strcmp(member, "DeactivateConnection") == 0) {
            g_nm_state = 30;
            g_nm_active_ap = "/";
            g_nm_ip4config = "/";
            g_nm_active_conn = "/";
            reply = empty_reply(msg);
        }
    } else if (strcmp(iface,
                      "org.freedesktop.NetworkManager.Device.Wireless") ==
               0) {
        if (strcmp(member, "RequestScan") == 0) {
            reply = empty_reply(msg);
        } else if (strcmp(member, "GetAccessPoints") == 0) {
            reply = dbus_message_new_method_return(msg);
            DBusMessageIter it, arr;
            dbus_message_iter_init_append(reply, &it);
            dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "o",
                                             &arr);
            const char *a1 = NM_AP1, *a2 = NM_AP2;
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_OBJECT_PATH,
                                           &a1);
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_OBJECT_PATH,
                                           &a2);
            dbus_message_iter_close_container(&it, &arr);
        }
    }

    if (!reply)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    const char *addr = getenv("RB_DBUS_ADDRESS");
    if (!addr || !*addr) addr = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = NULL;
    if (addr && *addr) {
        conn = dbus_connection_open_private(addr, &err);
        if (!conn) {
            fprintf(stderr, "fake_bus: open %s: %s\n", addr,
                    err.message ? err.message : err.name);
            return 1;
        }
        if (!dbus_bus_register(conn, &err)) {
            fprintf(stderr, "fake_bus: register: %s\n",
                    err.message ? err.message : err.name);
            return 1;
        }
    } else {
        conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (!conn) {
            fprintf(stderr, "fake_bus: system bus: %s\n",
                    err.message ? err.message : err.name);
            return 1;
        }
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    int r = dbus_bus_request_name(conn, "org.bluez",
                                  DBUS_NAME_FLAG_ALLOW_REPLACEMENT, &err);
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "fake_bus: cannot own org.bluez\n");
        return 1;
    }
    r = dbus_bus_request_name(conn, "org.freedesktop.NetworkManager",
                              DBUS_NAME_FLAG_ALLOW_REPLACEMENT, &err);
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "fake_bus: cannot own NetworkManager\n");
        return 1;
    }
    dbus_connection_add_filter(conn, filter_fn, NULL, NULL);
    /* Dispatch until the bus disconnects (parent kills us). */
    while (dbus_connection_read_write_dispatch(conn, 100))
        ;
    return 0;
}
