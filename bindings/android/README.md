# Android bindings — Phase 3 skeleton (not built)

Status: documented skeleton only. There is no Android SDK and no JDK
on the Phase 1 build machine, so nothing here compiles yet. This
directory exists so Phase 3 starts from a design, not a blank page.

## Requirements for the real binding

- Android SDK (API 31+ target for `BLUETOOTH_SCAN` /
  `BLUETOOTH_CONNECT`; API 21+ minimum for BLE central role).
- A device or emulator with Bluetooth hardware for anything beyond
  compilation.
- JNI bridge (`jni/rb_jni.c` sketch): a small Java helper class that
  owns `BluetoothLeScanner`, `BluetoothGatt`, `WifiManager`, and
  `ConnectivityManager` objects, and forwards their callbacks into C
  through global references.

## JNI rules the bridge must follow

- Every native callback runs on a binder thread: attach the thread
  (`AttachCurrentThread`) before touching JNI, detach when done, and
  never call back into Java while holding a RadioBridge lock.
- `BluetoothDevice`, `BluetoothGatt`, `ScanResult`, and `WifiInfo`
  objects are Java objects: the bridge converts them to portable
  `rb_ble_device_t` / `rb_wifi_network_t` structs immediately and
  releases local references. They must never cross into the C API.
- Scan errors map to the RadioBridge taxonomy:
  `SCAN_FAILED_ALREADY_STARTED` → `RB_ERROR_ALREADY_IN_PROGRESS`,
  `SCAN_FAILED_FEATURE_UNSUPPORTED` → `RB_ERROR_NOT_SUPPORTED`,
  `SCAN_FAILED_OUT_OF_HARDWARE_RESOURCES` → `RB_ERROR_BUSY`,
  `SCAN_FAILED_INTERNAL_ERROR` → `RB_ERROR_INTERNAL`.
- Permissions are reported, not requested, by the library:
  `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT` (API 31+),
  `ACCESS_FINE_LOCATION` for scanning pre-31, `ACCESS_WIFI_STATE` /
  `CHANGE_WIFI_STATE` for Wi-Fi. The app drives the runtime request.
- Wi-Fi scanning on modern Android is throttled and location-gated;
  the backend must surface that as `RB_ERROR_PERMISSION_DENIED` or a
  throttled empty result with a clear diagnostic — never a fake list.

## Files

- `jni/rb_jni.c` — JNI bridge sketch (not compiled).
- `java/RadioBridge.java` — Java wrapper sketch (not compiled).
- `java/RadioBridge.kt` — Kotlin wrapper sketch (not compiled).
