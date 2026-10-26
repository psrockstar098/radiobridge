# Backend architecture

One core, one backend vtable (`src/core/rb_internal.h`), one portable
struct surface. Backends differ only behind the vtable. The core owns
the context, the operation registry, logging, and event dispatch;
backends own native handles and worker threads.

## Layout

- `src/core/` — context, dispatch, logging, operations, capability
  snapshot, permission reporting, UUID helpers.
- `src/bluetooth/` — mock BLE backend (full implementation).
- `src/wifi/` — mock Wi-Fi backend (full implementation).
- `src/platform/linux/` — Linux backend: full BlueZ +
  NetworkManager implementation over libdbus (`rb_dbus.c`
  transport, `rb_linux.c` backend), verified against the fake
  service (Phase 2, done).
- `src/platform/windows/` — Windows backend: full WinRT
  implementation (C++/WinRT behind a C11 shim). COMPILE-UNVERIFIED
  — see the Windows section below.
- `src/platform/stubs/` — Android stub (probe + NOT_SUPPORTED
  only); the Windows stub survives only for non-Windows builds;
  Apple is retained only as an out-of-scope entry for ABI
  stability (see `src/platform/stubs/rb_stubs.c`).
- `src/app/` — application layer (Phase 5): registry, reliable
  transfer, firmware transport, secure channels, sessions,
  scheduler, power config, policy engine.
- `src/platform/android|windows/` — READMEs pointing at the
  stub + the Phase 2/3 work each needs.
- `src/events/`, `src/permissions/` — READMEs: the event system and
  permission reporting live in the core (`rb_core.c`) in Phase 1;
  these directories reserve the split for when they grow.

## Linux backend: D-Bus interface definitions (Phase 2 contract)

Declared as compiled constants in `src/platform/linux/rb_linux.c`:

**BlueZ** (`org.bluez`, system bus):

| Object | Path pattern | Interface | Key methods / properties |
| ------ | ------------ | --------- | ------------------------ |
| Adapter | `/org/bluez/hciX` | `org.bluez.Adapter1` | `StartDiscovery`, `StopDiscovery`, `Powered`, `Discovering` |
| Device | `/org/bluez/hciX/dev_XX_XX_XX_XX_XX_XX` | `org.bluez.Device1` | `Connect`, `Disconnect`, `Pair`; `Address`, `Name`, `RSSI`, `ManufacturerData`, `ServicesResolved`, `Connected`, `Paired` |
| GATT service | `.../serviceXXXX` | `org.bluez.GattService1` | `UUID`, `Primary` |
| GATT characteristic | `.../charXXXX` | `org.bluez.GattCharacteristic1` | `ReadValue`, `WriteValue` (offset; type `command`/`request`/`reliable`), `StartNotify`, `StopNotify`, `Value`, `Flags`, `Notifying` |
| GATT descriptor | `.../descXXXX` | `org.bluez.GattDescriptor1` | `ReadValue`, `WriteValue` |

BlueZ errors map to the taxonomy: `org.bluez.Error.Failed` →
`RB_ERROR_INTERNAL`, `.NotSupported` → `RB_ERROR_NOT_SUPPORTED`,
`.NotPermitted` → `RB_ERROR_PERMISSION_DENIED`, `.NotConnected` →
`RB_ERROR_NOT_CONNECTED`, `.InProgress` →
`RB_ERROR_ALREADY_IN_PROGRESS`, `.InvalidArguments` →
`RB_ERROR_INVALID_ARGUMENT`.

**NetworkManager** (`org.freedesktop.NetworkManager`):

| Object | Interface | Key methods / properties |
| ------ | --------- | ------------------------ |
| Root | `org.freedesktop.NetworkManager` | `GetDevices`, `ActivateConnection`, `AddAndActivateConnection`, `DeactivateConnection` |
| Wi-Fi device | `org.freedesktop.NetworkManager.Device.Wireless` | `RequestScan`, `GetAccessPoints`, `ActiveAccessPoint` |
| Access point | `org.freedesktop.NetworkManager.AccessPoint` | `Ssid` (bytes), `HwAddress`, `Strength` (0–100), `Flags`, `WpaFlags`, `RsnFlags`, `Mode`, `Frequency` |
| Settings | `org.freedesktop.NetworkManager.Settings` | `ListConnections`; per-connection `GetSettings`/`Update`/`Delete` |

## Phase 2 status (Linux) — DONE, fake-bus verified

1. libdbus transport linked (`src/platform/linux/rb_dbus.c`):
   bus-address resolution (`RB_DBUS_ADDRESS` override, then
   `DBUS_SYSTEM_BUS_ADDRESS`, then the system bus), method calls
   with timeout, `Properties.Get`, `GetManagedObjects` walking,
   error-name mapping (ServiceUnknown/NotReady → UNAVAILABLE,
   AccessDenied → PERMISSION_DENIED, timeout → TIMEOUT, …).
2. Capabilities are claimed only after `NameHasOwner("org.bluez")`
   / `NameHasOwner("org.freedesktop.NetworkManager")` checks; the
   probe detail names the literal evidence paths
   (`/sys/class/bluetooth`, `/run/dbus/system_bus_socket`,
   `/run/NetworkManager`).
3. Implemented: `Adapter1.StartDiscovery` + snapshot scans,
   `Device1.Connect` + `ServicesResolved` wait, GATT
   read/write/notify, `Device.Wireless.RequestScan` +
   `GetAccessPoints`, `AddAndActivateConnection` for Wi-Fi
   connect, `DeactivateConnection` for disconnect, TCP-connect
   quality probes.
4. Verification without hardware: `tests/test_linux_bus` starts a
   private `dbus-daemon` and `tests/fake_bus/fake_bus.c`, a fake
   BlueZ + NetworkManager service exposing the real interfaces
   with canned devices/networks. The backend passes its full test
   sequence against that fake service over the real bus protocol.
   That is genuine protocol verification, not a mock shortcut —
   but it is not a real-hardware run, and it does not claim to be.
5. When the services are absent, the backend keeps its honest
   `UNAVAILABLE` behavior (covered by `test_linux_backend`).

### Documented v0.2 simplifications (Linux)

- BLE scan is snapshot-based (StartDiscovery + settle +
  GetManagedObjects), not advertisement signals.
- BlueZ exposes no numeric GATT handles → synthetic sequential
  handles in snapshot order.
- BlueZ exposes no connectability → discovered devices are
  reported connectable.
- NetworkManager reports Strength percent (0–100), not dBm →
  Wi-Fi `signal_dbm` is 0 ("not reported"), never a fabricated
  percent→dBm mapping.
- AP security mapping is conservative (RsnFlags → WPA2, WPA3/SAE
  not distinguished; WpaFlags → WPA; privacy bit → WEP).
- Notifications are poll-based (~150 ms) after StartNotify; no
  signal consumption.
- Wi-Fi connect uses key-mgmt `wpa-psk` (covers WPA2 + WPA3
  transition); 802.1X enterprise is `NOT_SUPPORTED`.
- Reconnect policy is stored but auto-reconnect is not attempted
  (reconnect capability `UNAVAILABLE` on Linux).
- Quality is a TCP-connect probe, not ICMP or radio metrics.
- BLE connect is async; Wi-Fi connect is async with status
  polling; disconnect returns OK when already disconnected.

## Windows backend (Phase 4) — written, COMPILE-UNVERIFIED

`src/platform/windows/` (`rb_windows.c` shim, `rb_windows.cpp`
BLE, `rb_windows_wifi.cpp` Wi-Fi, `rb_windows_errors.md` error
tables + limits): advertisement scan via
`BluetoothLEAdvertisementWatcher`, connect via
`FromBluetoothAddressAsync` + `GattSession.MaintainConnection`,
GATT via `GetGattServicesAsync`/`GetCharacteristicsAsync`/
`ReadValueAsync`/`WriteValueAsync`, notify via `ValueChanged` +
CCCD write, pairing via `DeviceInformationPairing`, Wi-Fi via
`WiFiAdapter` (`RequestAccessAsync`, `ScanAsync`,
`AvailableNetworks`, `ConnectAsync`).

Honest limits (full list in `rb_windows_errors.md`): no explicit
disconnect primitive (disconnect is a request — the OS drops the
link only when no app holds the device); no RSSI read API
(`NOT_SUPPORTED` always, never fabricated); connection attempts
not cancellable; Wi-Fi needs the `wifiControl` manifest
capability — without it, `RequestAccessAsync` returns
`DeniedBySystem` (unpackaged Win32 apps have no manifest to
declare it in; exact unpackaged behavior needs on-device
confirmation); Wi-Fi security always `UNKNOWN` (no auth info
exposed); no IP/gateway in status; service handles are 0; a
residual shutdown race (connect workers blocked in uncancellable
WinRT calls are not joined on destroy) is flagged in the code.

## Android (Phase 3) — JNI bridge done, Framework helper blocked

The JNI bridge (`bindings/android/jni/rb_jni.c`) + Java wrapper
(`java/com/radiobridge/RadioBridge.java`) are compiled against a
plain JDK (no Android SDK, no `android.*` references) and proven
by `test_jni_smoke`: the JVM loads the built library and drives
the mock backend end to end (scan/connect/read). The Kotlin
wrapper (`RadioBridgeKt.kt`) is written but compile-unverified
(no kotlinc here). The Java helper that would call Android
Framework APIs (`BluetoothLeScanner`, `BluetoothGatt`,
`WifiManager`) cannot run without an Android device and does not
exist yet — the Android backend itself still reports
`NOT_SUPPORTED`. See `bindings/android/README.md`.

## Application layer (Phase 5) — `src/app/rb_app.c`

Public header `include/radiobridge/rb_app.h`. Device registry +
capability profiles (in-memory + hand-rolled JSON file, no new
dependency); reliable transfer (framing with SOH/length/sequence/
CRC32-IEEE, stop-and-wait retries, receiver dedup — sequences are
persistent per sender/receiver pair, never restarted per message,
so a stale ACK cannot falsely acknowledge a new message);
firmware transport (512-byte chunks, per-chunk SHA-256, manifest
hash; corrupt chunks rejected, re-sendable); secure channels
(X25519 ECDH → HKDF-SHA256 → AES-256-GCM via OpenSSL EVP; replay
rejected by nonce-sequence check; `NOT_SUPPORTED` without
OpenSSL); multi-device sessions (connect-all with per-device
results, broadcast write); operation scheduler (priority queue,
single-flight); power config (balanced/low-power/performance
presets + validation); policy engine (preferred/block lists,
RSSI floor, required services → ranked devices).

## Native error mapping (reference for Phase 2/3)

- **Android** `ScanCallback`: `ALREADY_STARTED` →
  `RB_ERROR_ALREADY_IN_PROGRESS`, `FEATURE_UNSUPPORTED` →
  `RB_ERROR_NOT_SUPPORTED`, `OUT_OF_HARDWARE_RESOURCES` →
  `RB_ERROR_BUSY`, `INTERNAL_ERROR` → `RB_ERROR_INTERNAL`.
  `BluetoothGatt` status codes map by the same table, with
  `GATT_INSUFFICIENT_AUTHENTICATION` → `RB_ERROR_AUTH_FAILED`.
- **WinRT**: `GattCommunicationStatus` `Success` → `RB_OK`,
  `Unreachable` → `RB_ERROR_NOT_FOUND` (or `TIMEOUT` after the
  platform's own ~7s queue wait), `AccessDenied` →
  `RB_ERROR_PERMISSION_DENIED`, `ProtocolError` →
  `RB_ERROR_INTERNAL`. No RSSI API exists: report `NOT_SUPPORTED`.
- **SimpleBLE lesson applied**: keep the error mapping in one table
  per backend, in the backend file, so a new platform error cannot
  silently become a generic failure.
