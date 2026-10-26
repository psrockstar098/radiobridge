// RadioBridge — Java wrapper (Phase 3).
//
// Thin Java class over the native library (see ../jni/rb_jni.c).
// Nothing here references android.* classes, so this compiles against
// a plain JDK; on Android the same wrapper drives the Android backend
// once its Framework helper exists. Permissions are reported by the
// library, never requested: the app drives the runtime request for
// BLUETOOTH_SCAN / BLUETOOTH_CONNECT (API 31+), ACCESS_FINE_LOCATION
// (scanning pre-31), ACCESS_WIFI_STATE / CHANGE_WIFI_STATE.
package com.radiobridge;

public final class RadioBridge implements AutoCloseable {
    static {
        System.loadLibrary("radiobridge_jni");
    }

    public interface ScanListener {
        void onDeviceFound(String address, String name, int rssiDbm);
    }

    public interface ConnectionListener {
        void onStateChanged(String address, int state, int status);
    }

    public static final int BACKEND_AUTO = 0;
    public static final int BACKEND_MOCK = 1;
    public static final int BACKEND_LINUX = 2;
    public static final int BACKEND_WINDOWS = 5;

    public static final int STATE_DISCONNECTED = 0;
    public static final int STATE_CONNECTING = 1;
    public static final int STATE_CONNECTED = 2;
    public static final int STATE_DISCONNECTING = 3;
    public static final int STATE_FAILED = 4;

    private long context;

    public RadioBridge(int backend) {
        context = nativeInitialize(backend);
        if (context == 0) {
            throw new IllegalStateException("rb_initialize failed");
        }
    }

    @Override
    public void close() {
        if (context != 0) {
            nativeShutdown(context);
            context = 0;
        }
    }

    public static String version() {
        return nativeVersion();
    }

    public String capabilities() {
        checkOpen();
        return nativeCapabilities(context);
    }

    /** Starts a scan; returns an operation id for bleScanStop/cancel. */
    public long bleScanStart(ScanListener listener) {
        checkOpen();
        long op = nativeBleScanStart(context, listener);
        if (op == 0) {
            throw new IllegalStateException("rb_ble_scan_start failed");
        }
        return op;
    }

    /** Returns the rb_status_t of the stop (0 = OK). */
    public int bleScanStop(long operation) {
        checkOpen();
        return nativeBleScanStop(context, operation);
    }

    /** Returns an operation id; the outcome arrives via the listener. */
    public long bleConnect(String address, ConnectionListener listener) {
        checkOpen();
        long op = nativeBleConnect(context, address, listener);
        if (op == 0) {
            throw new IllegalStateException("rb_ble_connect failed");
        }
        return op;
    }

    /** Returns the rb_status_t of the disconnect (0 = OK). */
    public int bleDisconnect(String address) {
        checkOpen();
        return nativeBleDisconnect(context, address);
    }

    /**
     * Reads a characteristic. Returns null when the read fails
     * (not connected, unknown device/characteristic, or the
     * characteristic is not readable). Use the C API for status codes.
     */
    public byte[] bleRead(String address, String serviceUuid,
                          String characteristicUuid) {
        checkOpen();
        return nativeBleRead(context, address, serviceUuid,
                             characteristicUuid);
    }

    /** "state|ssid|ip" (state: 0 disconnected, 1 connecting, 2 connected). */
    public String wifiStatus() {
        checkOpen();
        return nativeWifiStatus(context);
    }

    /** Returns the rb_status_t of the cancel (0 = OK). */
    public int cancel(long operation) {
        checkOpen();
        return nativeCancel(context, operation);
    }

    private void checkOpen() {
        if (context == 0) {
            throw new IllegalStateException("RadioBridge is closed");
        }
    }

    private static native long nativeInitialize(int backend);

    private static native void nativeShutdown(long context);

    private static native String nativeVersion();

    private static native String nativeCapabilities(long context);

    private static native long nativeBleScanStart(long context,
                                                  ScanListener listener);

    private static native int nativeBleScanStop(long context,
                                                long operation);

    private static native long nativeBleConnect(long context,
                                                String address,
                                                ConnectionListener listener);

    private static native int nativeBleDisconnect(long context,
                                                  String address);

    private static native byte[] nativeBleRead(long context,
                                               String address,
                                               String serviceUuid,
                                               String characteristicUuid);

    private static native String nativeWifiStatus(long context);

    private static native int nativeCancel(long context, long operation);
}
