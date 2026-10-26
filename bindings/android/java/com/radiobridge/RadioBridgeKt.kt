// RadioBridge — Kotlin wrapper (Phase 3).
//
// Thin Kotlin facade over the Java wrapper. Callback lambdas replace
// the Java listener interfaces; everything else delegates 1:1.
//
// NOTE: compile-unverified on machines without kotlinc. The Java
// wrapper + JNI smoke test (TestSmoke.java) are the compiled proof;
// this file is written in straightforward Kotlin with no Android
// dependencies, and compiles with `kotlinc` against the Java wrapper.
package com.radiobridge

class RadioBridgeKt(backend: Int) : AutoCloseable {
    private val inner = RadioBridge(backend)

    fun capabilities(): String = inner.capabilities()

    fun bleScanStart(
        onDeviceFound: (address: String, name: String, rssiDbm: Int) -> Unit
    ): Long = inner.bleScanStart { address, name, rssiDbm ->
        onDeviceFound(address, name, rssiDbm)
    }

    fun bleScanStop(operation: Long): Int = inner.bleScanStop(operation)

    fun bleConnect(
        address: String,
        onStateChanged: (address: String, state: Int, status: Int) -> Unit
    ): Long = inner.bleConnect(address) { a, state, status ->
        onStateChanged(a, state, status)
    }

    fun bleDisconnect(address: String): Int = inner.bleDisconnect(address)

    fun bleRead(
        address: String,
        serviceUuid: String,
        characteristicUuid: String
    ): ByteArray? = inner.bleRead(address, serviceUuid, characteristicUuid)

    fun wifiStatus(): String = inner.wifiStatus()

    fun cancel(operation: Long): Int = inner.cancel(operation)

    override fun close() = inner.close()

    companion object {
        fun version(): String = RadioBridge.version()
    }
}
