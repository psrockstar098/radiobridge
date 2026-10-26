// RadioBridge — Java wrapper SKETCH (Phase 3, NOT COMPILED).
//
// Requires the Android SDK. This sketch shows the wrapper shape only:
// a thin Java class over the native library, with callback interfaces
// mirroring the C callbacks. The real implementation wires each method
// to rb_* via the JNI bridge (see ../jni/rb_jni.c).
//
//   public final class RadioBridge {
//       static { System.loadLibrary("radiobridge"); }
//       public interface ScanListener { void onDeviceFound(...); }
//       public interface ConnectionListener { void onStateChanged(...); }
//       public native long initialize();
//       public native void shutdown(long context);
//       public native long bleScanStart(long context, ...);
//       public native void cancel(long context, long operationId);
//       ...
//   }
//
// Permissions the app must request (the library only reports them):
//   BLUETOOTH_SCAN, BLUETOOTH_CONNECT (API 31+),
//   ACCESS_FINE_LOCATION (scanning pre-31),
//   ACCESS_WIFI_STATE, CHANGE_WIFI_STATE.
