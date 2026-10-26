/*
 * RadioBridge — rb_linux.c (Linux backend, Phase 1/2 scope)
 *
 * STRUCTURE + INTERFACE DEFINITIONS + HONEST UNAVAILABLE REPORTING.
 * This backend does NOT yet speak D-Bus: the VM has no libdbus headers,
 * no bluetoothd, and no NetworkManager, so a D-Bus transport cannot be
 * built or tested here. What this file does, verifiably:
 *
 *  1. Probes for the platform services (adapter sysfs, D-Bus system bus
 *     socket, NetworkManager runtime dir) and reports exactly what is
 *     missing.
 *  2. Declares the real BlueZ and NetworkManager D-Bus interface
 *     definitions (service names, object paths, interfaces, methods,
 *     properties, errors) as compiled constants, so Phase 2 wires a
 *     transport onto them without redesigning the backend.
 *  3. Returns RB_ERROR_UNAVAILABLE — never fabricated scan results,
 *     connections, or networks — for every operation until the
 *     transport lands.
 *
 * Phase 2 work (see docs/backends.md): link libdbus (or a minimal
 * hand-rolled D-Bus client), confirm daemon presence with
 * NameHasOwner("org.bluez") / NameHasOwner("org.freedesktop.
 * NetworkManager"), then implement Adapter1/Device1/Gatt calls and
 * NM Device.Wireless calls against the definitions below.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "core/rb_internal.h"

/* ------------------------------------------------------------------ */
/* BlueZ D-Bus interface definitions (grounded in the BlueZ D-Bus API */
/* documentation and the GattCharacteristic1 man page).                */
/* ------------------------------------------------------------------ */

/* Service + object manager. */
static const char *RB_BLUEZ_SERVICE = "org.bluez";
static const char *RB_DBUS_OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";
static const char *RB_DBUS_PROPERTIES = "org.freedesktop.DBus.Properties";

/* Adapter: /org/bluez/hci0 — Adapter1: StartDiscovery, StopDiscovery,
 * Powered, Discovering, Address. */
static const char *RB_BLUEZ_ADAPTER_IFACE = "org.bluez.Adapter1";
static const char *RB_BLUEZ_ADAPTER_PATH_FMT = "/org/bluez/hci%d";

/* Device: /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX — Device1: Connect,
 * Disconnect, Pair, CancelPairing; properties: Address, Name, RSSI,
 * ManufacturerData, ServiceData, ServicesResolved, Connected, Paired. */
static const char *RB_BLUEZ_DEVICE_IFACE = "org.bluez.Device1";

/* GATT (remote/client role): serviceXXXX / charXXXX / descXXXX under
 * the device path.
 * GattService1: UUID, Primary, Characteristics.
 * GattCharacteristic1: UUID, Service, Value, Flags, Notifying;
 *   ReadValue(dict{offset, mtu}), WriteValue(bytes, dict{offset, type,
 *   mtu}) with type "command" | "request" | "reliable";
 *   StartNotify, StopNotify, AcquireWrite, AcquireNotify.
 * GattDescriptor1: UUID, Characteristic, Value; ReadValue, WriteValue.
 * BlueZ errors: org.bluez.Error.Failed, .NotSupported, .NotPermitted,
 *   .NotConnected, .InProgress, .InvalidArguments. */
static const char *RB_BLUEZ_GATT_SERVICE_IFACE = "org.bluez.GattService1";
static const char *RB_BLUEZ_GATT_CHAR_IFACE =
    "org.bluez.GattCharacteristic1";
static const char *RB_BLUEZ_GATT_DESC_IFACE = "org.bluez.GattDescriptor1";

/* ------------------------------------------------------------------ */
/* NetworkManager D-Bus interface definitions (grounded in the NM      */
/* D-Bus API: Device.Wireless RequestScan/GetAccessPoints,             */
/* ActivateConnection, Settings.ListConnections).                      */
/* ------------------------------------------------------------------ */

/* Service: org.freedesktop.NetworkManager.
 * Root methods: GetDevices, ActivateConnection, AddAndActivateConnection,
 * DeactivateConnection.
 * Device.Wireless: RequestScan, GetAccessPoints, ActiveAccessPoint.
 * AccessPoint properties: Ssid (bytes), HwAddress, Strength (0-100),
 * Flags, WpaFlags, RsnFlags, Mode, Frequency.
 * Settings: ListConnections; Connection: GetSettings, Update, Delete. */
static const char *RB_NM_SERVICE = "org.freedesktop.NetworkManager";
static const char *RB_NM_WIRELESS_IFACE =
    "org.freedesktop.NetworkManager.Device.Wireless";
static const char *RB_NM_AP_IFACE =
    "org.freedesktop.NetworkManager.AccessPoint";

/* Probe evidence paths (filesystem only — no D-Bus client yet). */
#define RB_LINUX_BT_SYSFS "/sys/class/bluetooth"
#define RB_LINUX_DBUS_SOCKET "/run/dbus/system_bus_socket"
#define RB_LINUX_NM_RUNDIR "/run/NetworkManager"

static void rb_linux_build_detail(char *detail, size_t len, int has_adapter,
                                  int has_bus, int has_nm) {
    /* Reference the interface constants so they stay compiled in and
     * grep-able as the Phase 2 contract (avoids unused warnings). */
    const char *ifaces = RB_BLUEZ_SERVICE;
    (void)RB_DBUS_OBJECT_MANAGER;
    (void)RB_DBUS_PROPERTIES;
    (void)RB_BLUEZ_ADAPTER_IFACE;
    (void)RB_BLUEZ_ADAPTER_PATH_FMT;
    (void)RB_BLUEZ_DEVICE_IFACE;
    (void)RB_BLUEZ_GATT_SERVICE_IFACE;
    (void)RB_BLUEZ_GATT_CHAR_IFACE;
    (void)RB_BLUEZ_GATT_DESC_IFACE;
    (void)RB_NM_SERVICE;
    (void)RB_NM_WIRELESS_IFACE;
    (void)RB_NM_AP_IFACE;
    (void)ifaces;
    snprintf(detail, len,
             "linux backend: BlueZ adapter %s (%s), D-Bus system bus %s "
             "(%s), NetworkManager %s (%s); D-Bus transport not "
             "implemented in Phase 1/2 scope — every operation reports "
             "unavailable, never fabricated results",
             has_adapter ? "present" : "absent", RB_LINUX_BT_SYSFS,
             has_bus ? "present" : "absent", RB_LINUX_DBUS_SOCKET,
             has_nm ? "present" : "absent", RB_LINUX_NM_RUNDIR);
}

rb_status_t rb_linux_probe(rb_capabilities_t *caps) {
    if (caps == NULL) return RB_ERROR_INVALID_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
    rb_copy_str(caps->backend_name, sizeof(caps->backend_name), "linux");
    rb_copy_str(caps->backend_version, sizeof(caps->backend_version),
                "0.1.0");
    rb_copy_str(caps->os_name, sizeof(caps->os_name), "linux");
    int has_adapter = rb_path_exists(RB_LINUX_BT_SYSFS);
    int has_bus = rb_path_exists(RB_LINUX_DBUS_SOCKET);
    int has_nm = rb_path_exists(RB_LINUX_NM_RUNDIR);
    /* Phase 1/2 scope: no transport, so every feature is UNAVAILABLE
     * regardless of what the probe finds. The detail string is the
     * diagnostic that tells Phase 2 what to wire up. */
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
    rb_linux_build_detail(caps->detail, sizeof(caps->detail), has_adapter,
                          has_bus, has_nm);
    return RB_OK;
}

static rb_status_t linux_create(rb_context_t *ctx) {
    /* The backend cannot perform any operation in this phase, so an
     * explicit linux context fails fast with UNAVAILABLE. AUTO callers
     * fall back to mock (see rb_initialize). The probe detail is kept
     * on the context for diagnostics. */
    rb_capabilities_t caps;
    rb_linux_probe(&caps);
    rb_logf(ctx, RB_LOG_WARN, "linux", "%s", caps.detail);
    ctx->caps = caps;
    return RB_ERROR_UNAVAILABLE;
}

static void linux_destroy(rb_context_t *ctx) {
    ctx->backend_state = NULL;
}

/* Every operation reports unavailable: no transport, no fabrication. */
static rb_status_t linux_ble_scan_start(rb_context_t *ctx, const rb_ble_scan_options_t *o, rb_ble_scan_callback_t c, void *u, rb_operation_id_t op) { (void)ctx; (void)o; (void)c; (void)u; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_scan_stop(rb_context_t *ctx, rb_operation_id_t op) { (void)ctx; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_connect(rb_context_t *ctx, const rb_device_id_t *d, const rb_ble_connect_options_t *o, rb_ble_connection_callback_t c, void *u, rb_operation_id_t op) { (void)ctx; (void)d; (void)o; (void)c; (void)u; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_disconnect(rb_context_t *ctx, const rb_device_id_t *d) { (void)ctx; (void)d; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_discover(rb_context_t *ctx, const rb_device_id_t *d, rb_ble_service_t **o, size_t *n) { (void)ctx; (void)d; (void)o; (void)n; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_read(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c, uint8_t *v, size_t *n) { (void)ctx; (void)d; (void)s; (void)c; (void)v; (void)n; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_write(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c, const uint8_t *v, size_t n, int w) { (void)ctx; (void)d; (void)s; (void)c; (void)v; (void)n; (void)w; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_subscribe(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c, rb_ble_notify_callback_t cb, void *u) { (void)ctx; (void)d; (void)s; (void)c; (void)cb; (void)u; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_unsubscribe(rb_context_t *ctx, const rb_device_id_t *d, const rb_uuid_t *s, const rb_uuid_t *c) { (void)ctx; (void)d; (void)s; (void)c; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_get_rssi(rb_context_t *ctx, const rb_device_id_t *d, int8_t *r) { (void)ctx; (void)d; (void)r; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_ble_set_reconnect_policy(rb_context_t *ctx, const rb_reconnect_policy_t *p) { (void)ctx; (void)p; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_wifi_get_status(rb_context_t *ctx, rb_wifi_status_t *o) { (void)ctx; (void)o; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_wifi_scan_start(rb_context_t *ctx, const rb_wifi_scan_options_t *o, rb_wifi_scan_callback_t c, void *u, rb_operation_id_t op) { (void)ctx; (void)o; (void)c; (void)u; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_wifi_scan_stop(rb_context_t *ctx, rb_operation_id_t op) { (void)ctx; (void)op; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_wifi_connect(rb_context_t *ctx, const char *s, const char *p, const rb_wifi_connect_options_t *o) { (void)ctx; (void)s; (void)p; (void)o; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_wifi_disconnect(rb_context_t *ctx) { (void)ctx; return RB_ERROR_UNAVAILABLE; }
static rb_status_t linux_wifi_measure_quality(rb_context_t *ctx, const char *e, rb_wifi_quality_t *o) { (void)ctx; (void)e; (void)o; return RB_ERROR_UNAVAILABLE; }

static rb_status_t linux_required_permissions(rb_operation_t op, rb_permission_mask_t *mask) {
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
    .version = "0.1.0",
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
};
