/*
 * RadioBridge — rb_stubs.c (Android / Windows backends; Apple
 * retained out-of-scope only, see below)
 *
 * Phase 1/2 scope: documented stubs, NOT implementations. Each stub
 * reports every feature as RB_CAP_NOT_SUPPORTED with a detail string
 * naming the SDK + hardware its real backend needs, and its create()
 * fails with RB_ERROR_NOT_SUPPORTED, so no caller can mistake a stub
 * for a working radio. All operation pointers are NULL: the core maps
 * a NULL op to RB_ERROR_NOT_SUPPORTED.
 *
 * What each real backend needs (see docs/backends.md):
 *  - Android: Android SDK + a device/emulator with Bluetooth; JNI
 *    bridge to BluetoothLeScanner / BluetoothGatt / WifiManager /
 *    ConnectivityManager. JNI patterns (global refs, thread attach)
 *    are sketched in bindings/android/jni/rb_jni.c.
 *  - Apple: OUT OF SCOPE since 2026-10-26 (dropped from the
 *    project). No Apple backend or bindings exist; apple_probe is
 *    retained only so RB_BACKEND_APPLE keeps reporting
 *    NOT_SUPPORTED for ABI stability.
 *  - Windows: Windows SDK + a Windows device; WinRT
 *    BluetoothLEAdvertisementWatcher / BluetoothLEDevice /
 *    GattDeviceService / GattCharacteristic, plus WLAN APIs. Known
 *    WinRT limits that will shape the backend: no explicit disconnect,
 *    no RSSI read, no MTU request, connection attempts not cancellable.
 */
#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include "core/rb_internal.h"

static rb_status_t stub_probe_named(const char *name, const char *detail,
                                    rb_capabilities_t *caps) {
    if (caps == NULL) return RB_ERROR_INVALID_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
    rb_copy_str(caps->backend_name, sizeof(caps->backend_name), name);
    rb_copy_str(caps->backend_version, sizeof(caps->backend_version),
                "0.1.0");
    rb_copy_str(caps->os_name, sizeof(caps->os_name), name);
    caps->ble.scan = RB_CAP_NOT_SUPPORTED;
    caps->ble.connect = RB_CAP_NOT_SUPPORTED;
    caps->ble.gatt = RB_CAP_NOT_SUPPORTED;
    caps->ble.notify = RB_CAP_NOT_SUPPORTED;
    caps->ble.pairing = RB_CAP_NOT_SUPPORTED;
    caps->ble.rssi = RB_CAP_NOT_SUPPORTED;
    caps->ble.reconnect = RB_CAP_NOT_SUPPORTED;
    caps->wifi.status = RB_CAP_NOT_SUPPORTED;
    caps->wifi.scan = RB_CAP_NOT_SUPPORTED;
    caps->wifi.connect = RB_CAP_NOT_SUPPORTED;
    caps->wifi.quality = RB_CAP_NOT_SUPPORTED;
    caps->wifi.saved_networks = RB_CAP_NOT_SUPPORTED;
    rb_copy_str(caps->detail, sizeof(caps->detail), detail);
    return RB_OK;
}

static rb_status_t android_probe(rb_capabilities_t *caps) {
    return stub_probe_named(
        "android",
        "android backend: stub in Phase 1/2 scope — requires the Android "
        "SDK, a device with Bluetooth, and the JNI bridge (Phase 3); "
        "no Android APIs are callable from this Linux VM",
        caps);
}

static rb_status_t apple_probe(rb_capabilities_t *caps) {
    return stub_probe_named(
        "apple",
        "apple backend: OUT OF SCOPE since 2026-10-26 — no Apple "
        "backend or bindings exist in this project",
        caps);
}

static rb_status_t windows_probe(rb_capabilities_t *caps) {
    return stub_probe_named(
        "windows",
        "windows backend: stub in Phase 1/2 scope — requires the Windows "
        "SDK and a Windows device for WinRT Bluetooth/WLAN APIs; no "
        "Windows SDK exists on this Linux VM",
        caps);
}

static rb_status_t stub_create(rb_context_t *ctx) {
    (void)ctx;
    return RB_ERROR_NOT_SUPPORTED;
}

static void stub_destroy(rb_context_t *ctx) {
    ctx->backend_state = NULL;
}

static rb_status_t stub_permissions(rb_operation_t op,
                                    rb_permission_mask_t *mask) {
    (void)op;
    if (mask == NULL) return RB_ERROR_INVALID_ARGUMENT;
    *mask = RB_PERM_NONE;
    return RB_OK;
}

const rb_backend_ops_t rb_android_stub_backend_ops = {
    .name = "android",
    .version = "0.1.0",
    .probe = android_probe,
    .create = stub_create,
    .destroy = stub_destroy,
    .required_permissions = stub_permissions,
};

const rb_backend_ops_t rb_apple_stub_backend_ops = {
    .name = "apple",
    .version = "0.1.0",
    .probe = apple_probe,
    .create = stub_create,
    .destroy = stub_destroy,
    .required_permissions = stub_permissions,
};

const rb_backend_ops_t rb_windows_stub_backend_ops = {
    .name = "windows",
    .version = "0.1.0",
    .probe = windows_probe,
    .create = stub_create,
    .destroy = stub_destroy,
    .required_permissions = stub_permissions,
};
