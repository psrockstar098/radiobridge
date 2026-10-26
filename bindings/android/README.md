# Android bindings — Phase 3 (JNI bridge compiled + smoke-tested)

Status: the JNI bridge and Java wrapper are COMPILED and PROVEN by
a real JVM smoke test. The Android Framework backend (the Java
helper that would call Android Framework APIs) is HONESTLY BLOCKED:
it needs an Android SDK + device and does not exist yet.

## What is done (proven)

- `jni/rb_jni.c` — real JNI bridge, compiled against a plain JDK
  (no Android SDK, no `android.*` references). `JNI_OnLoad` caches
  the JavaVM; worker-thread callbacks attach/detach around every
  JNI call; no RadioBridge lock is ever held across a JNI call;
  listeners are global refs freed only when no callback can be in
  flight (scan records freed by `nativeBleScanStop` /
  `nativeShutdown`; connect records free themselves on terminal
  events); Java exceptions from callbacks are cleared, never
  propagated into C worker threads.
- `java/com/radiobridge/RadioBridge.java` — Java wrapper
  (`AutoCloseable`): backend constants, scan/connect listeners,
  `capabilities()`, `bleScanStart/Stop`, `bleConnect/Disconnect`,
  `bleRead`, `wifiStatus()`, `cancel()`. Permissions are reported
  by the library, never requested (the app drives the runtime
  request for `BLUETOOTH_SCAN`/`BLUETOOTH_CONNECT` on API 31+,
  `ACCESS_FINE_LOCATION` pre-31, `ACCESS_WIFI_STATE` /
  `CHANGE_WIFI_STATE`).
- `java/com/radiobridge/TestSmoke.java` — JVM smoke test, run by
  CTest as `test_jni_smoke`: loads the built library, drives the
  mock backend end to end (version, capabilities, scan → 3
  devices, connect → CONNECTED, battery read → `[87]`,
  notify-only read → null, Wi-Fi status disconnected, disconnect).
- `java/com/radiobridge/RadioBridgeKt.kt` — Kotlin facade with
  lambda callbacks over the Java wrapper. WRITTEN,
  COMPILE-UNVERIFIED (no kotlinc on the build machine); the Java
  wrapper + smoke test are the compiled proof.

## Building

CMake option `RB_BUILD_JNI` (default ON, non-Windows/non-Apple).
The bridge builds when `javac` + `java` + `jni.h` are found;
otherwise it is skipped, never failed. Note: CMake's own FindJNI
is deliberately NOT used — it requires AWT headers (`jawt.h`),
which headless JDKs do not ship, and the bridge never touches
AWT. `jni.h` is located directly from the resolved JVM home (plus
`$JAVA_HOME` as a hint), so headless JDKs work.

## JNI rules the bridge follows

- Every native callback runs on a worker thread: attach the thread
  (`AttachCurrentThread`) before touching JNI, detach when done, and
  never call back into Java while holding a RadioBridge lock.
- Scan/connect records own their callback arguments and free them
  only when no callback can be in flight (see above).
- Java exceptions from listener callbacks are cleared, never
  propagated into C worker threads.
- `nativeBleRead` returns `byte[]` or `null` on any failure — no
  status code at this layer (documented in the wrapper).
- `nativeWifiStatus` returns `"state|ssid|ip"` with state ints
  0 DISCONNECTED / 1 CONNECTING / 2 CONNECTED / 3 UNAVAILABLE /
  4 PERMISSION_DENIED / 5 FAILED.

## What is honestly blocked

- **Android Framework helper**: a Java helper class owning
  `BluetoothLeScanner`, `BluetoothGatt`, `WifiManager`, and
  `ConnectivityManager` objects and forwarding their callbacks
  into C through global references. Requires an Android SDK +
  a device or emulator with Bluetooth hardware. Until it exists,
  the Android backend reports `NOT_SUPPORTED` and the JNI bridge
  drives other backends (mock in the smoke test).
- **Scan error mapping** (for the future helper):
  `SCAN_FAILED_ALREADY_STARTED` → `RB_ERROR_ALREADY_IN_PROGRESS`,
  `SCAN_FAILED_FEATURE_UNSUPPORTED` → `RB_ERROR_NOT_SUPPORTED`,
  `SCAN_FAILED_OUT_OF_HARDWARE_RESOURCES` → `RB_ERROR_BUSY`,
  `SCAN_FAILED_INTERNAL_ERROR` → `RB_ERROR_INTERNAL`.
- **Wi-Fi notes for the future helper**: Wi-Fi scanning on modern
  Android is throttled and location-gated; the backend must surface
  that as `RB_ERROR_PERMISSION_DENIED` or a throttled empty result
  with a clear diagnostic — never a fake list.

## Files

- `jni/rb_jni.c` — JNI bridge (compiled, smoke-tested).
- `java/com/radiobridge/RadioBridge.java` — Java wrapper (compiled).
- `java/com/radiobridge/RadioBridgeKt.kt` — Kotlin wrapper
  (written, compile-unverified).
- `java/com/radiobridge/TestSmoke.java` — JVM smoke test.
