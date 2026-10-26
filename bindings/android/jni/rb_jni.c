/*
 * RadioBridge — Android JNI bridge SKETCH (Phase 3, NOT COMPILED).
 *
 * This file documents the JNI design only. It requires the Android SDK
 * and a device; it is deliberately excluded from CMakeLists.txt.
 *
 * Design (mirrors the SimpleBLE Android backend shape: a Java helper
 * class owns the Android objects, C++ / C code talks to it via JNI):
 *
 *  Java side (helper class, see ../java/RadioBridge.java):
 *    - BluetoothLeScanner.startScan(filters, settings, ScanCallback)
 *    - BluetoothDevice.connectGatt(context, false, BluetoothGattCallback)
 *    - BluetoothGatt.discoverServices / readCharacteristic /
 *      writeCharacteristic / setCharacteristicNotification /
 *      readRemoteRssi
 *    - WifiManager.startScan / getScanResults / getConnectionInfo
 *    - ConnectivityManager.registerNetworkCallback
 *
 *  Native side (this file, to be written in Phase 3):
 *    - JNI_OnLoad: cache the helper class + method IDs as global refs.
 *    - Each rb_* call marshals to the helper on the calling thread;
 *      each Java callback re-enters C via a native method that copies
 *      the result into rb_* structs and invokes the user's C callback.
 *    - Threads: attach on entry (AttachCurrentThread), detach on exit;
 *      never hold a RadioBridge mutex across a JNI call into Java.
 *    - References: NewGlobalRef for the helper + callback objects kept
 *      across calls; DeleteLocalRef for every per-call object.
 *    - Errors: ScanCallback error codes and BluetoothGatt status codes
 *      map to rb_status_t per docs/backends.md.
 *
 * What must NOT happen: passing BluetoothDevice / BluetoothGatt /
 * ScanResult objects into the C API, or exposing them in any public
 * header. The C API only ever sees rb_ble_device_t and friends.
 */
