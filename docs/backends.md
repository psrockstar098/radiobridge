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
- `src/platform/linux/` — Linux backend: probe + D-Bus interface
  definitions + unavailable reporting (Phase 1/2 scope).
- `src/platform/stubs/` — Android/Windows stubs (probe +
  NOT_SUPPORTED only); Apple is retained only as an out-of-scope
  entry for ABI stability (see `src/platform/stubs/rb_stubs.c`).
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

## Phase 2 checklist (Linux)

1. Link a D-Bus client (libdbus dev headers, or a minimal
   hand-rolled client — decision open; libdbus is the proven path,
   the same one SimpleBLE's SimpleDBus layer takes).
2. Confirm daemon presence with `NameHasOwner("org.bluez")` and
   `NameHasOwner("org.freedesktop.NetworkManager")` before claiming
   any capability — the current filesystem probe is only a Phase 1
   approximation.
3. Implement `Adapter1.StartDiscovery` + `ObjectManager` signals for
   scan; `Device1.Connect` + `ServicesResolved` wait for connect;
   `GattCharacteristic1` calls for GATT; `Device.Wireless.RequestScan`
   + `GetAccessPoints` for Wi-Fi scan; `ActivateConnection` for
   Wi-Fi connect.
4. Add a hardware-gated integration test (see testing.md) that runs
   only when an adapter is present; it must skip — never fake — on
   adapter-less machines.
5. Replace the filesystem probe with the D-Bus probe; keep the
   `UNAVAILABLE` behavior when services are absent.

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
