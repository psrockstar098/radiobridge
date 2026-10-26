// RadioBridge — Kotlin wrapper SKETCH (Phase 3, NOT COMPILED).
//
// Requires the Android SDK. Kotlin-friendly facade over the same native
// library: suspend-friendly callbacks via Flow/StateFlow are a Phase 3
// design decision, not a Phase 1 promise. Shape:
//
//   class RadioBridge(context: Context) : Closeable {
//       fun scanDevices(): Flow<Device>
//       suspend fun connect(device: Device): Connection
//       fun wifiStatus(): Flow<WifiStatus>
//       override fun close()
//   }
//
// See ../java/RadioBridge.java for the Java shape and ../README.md for
// the permission and threading rules the wrappers must follow.
