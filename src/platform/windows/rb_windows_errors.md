<!--
COMPILE-UNVERIFIED — written from documented WinRT APIs, never
compiled (no Windows SDK on the authoring machine). Requires
Windows SDK + a Bluetooth-capable Windows device to verify.
-->

# Windows backend — error mapping, capabilities, limitations

This file is the integration companion to the Windows backend in this
directory. It is intentionally separate from the public docs: the
integrator (who wires these files into the build) needs the exact
CMake snippet, the exact core changes, and the honest list of what
was verified against Microsoft Learn and what was not.

No Apple-related code was written in this directory. Apple is out of
scope by the project owner's 2026-10-26 decision; nothing here
references Core Bluetooth, Xcode, or any Apple SDK.

## Files in this directory

| File | Contents |
| --- | --- |
| `rb_windows_internal.h` | C-linkage bridge (C shim <-> C++ implementation). No WinRT types. |
| `rb_windows_state.h` | C++-only shared state: `WinState`, connection/scan/subscription records, the synchronous-call `SharedState`, and helper declarations. |
| `rb_windows.cpp` | BLE: advertisement scan, connect (with managed reconnect), GATT discover/read/write/subscribe/unsubscribe, pairing helpers, probe, create/destroy. |
| `rb_windows_wifi.cpp` | Wi-Fi: adapter probe, status via the connectivity profile, scan loop, connect worker, disconnect. |
| `rb_windows.c` | C11 vtable shim (`rb_windows_backend_ops`). Answers `ble_get_rssi` and `wifi_measure_quality` as NOT_SUPPORTED without touching WinRT. |
| `rb_windows_errors.md` | This file. |

## Language choice

**C++ with C++/WinRT behind an `extern "C"` shim** (option (a) from
the task). Rationale:

- The library's public API stays C11; only these Windows-only
  translation units compile as C++17.
- The raw-COM alternative would mean hand-writing every WinRT
  interface vtable by hand — including `IAsyncOperation`,
  `TypedEventHandler`, and `IVectorView` machinery — with no
  projection to check names against. That is strictly more
  error-prone, and it is not what real projects ship: SimpleBLE,
  the reference cross-platform BLE library for this exact problem,
  uses C++/WinRT for its Windows backend.
- C++/WinRT is header-only (Windows SDK or the
  `Microsoft.Windows.CppWinRT` NuGet package). No extra runtime
  dependency is introduced.

## CMake integration snippet (for the integrator — NOT applied here)

```cmake
if(WIN32)
  # The library is C11 everywhere else; the Windows backend alone
  # needs C++17 for the C++/WinRT projection.
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  target_sources(radiobridge PRIVATE
    src/platform/windows/rb_windows.c
    src/platform/windows/rb_windows.cpp
    src/platform/windows/rb_windows_wifi.cpp)

  # WinRT API surface. Prefer the Microsoft.Windows.CppWinRT NuGet
  # package when the SDK projection is not on the include path;
  # otherwise the Windows SDK headers suffice.
  target_link_libraries(radiobridge PRIVATE windowsapp)
endif()
```

Required core changes (also NOT applied here — this task forbids
touching anything outside `src/platform/windows/`):

1. `src/core/rb_internal.h`: add
   `extern const rb_backend_ops_t rb_windows_backend_ops;`
2. `src/core/rb_core.c`, `rb_ops_for_kind()`: point
   `RB_BACKEND_WINDOWS` at `&rb_windows_backend_ops` instead of
   `&rb_windows_stub_backend_ops`. In the `RB_BACKEND_AUTO` branch,
   add a `_WIN32` arm mirroring the `__linux__` arm (try the
   Windows backend, fall back to mock with a warning when it is
   unavailable).
3. `src/platform/stubs/rb_stubs.c`: delete the
   `rb_windows_stub_backend_ops` definition (keep the Android and
   Apple stubs). Nothing else references it: the only references
   in the tree are `rb_internal.h`, `rb_core.c`, and `rb_stubs.c`
   itself (verified by grep during authoring).

## Capability coverage

| Feature | WinRT API | Reported state |
| --- | --- | --- |
| BLE scan | `BluetoothLEAdvertisementWatcher` (Active mode), `Received`, `RawSignalStrengthInDBm`, `Advertisement.LocalName/ServiceUuids/ManufacturerData/DataSections` | AVAILABLE when a Bluetooth adapter exists, else UNAVAILABLE |
| BLE connect | `BluetoothLEDevice::FromBluetoothAddressAsync`, uncached `GetGattServicesAsync`, `GattSession.MaintainConnection`, `ConnectionStatusChanged` | AVAILABLE / UNAVAILABLE (same gate) |
| BLE GATT discover | `GetGattServicesAsync(Uncached)`, `GattDeviceService::GetCharacteristicsAsync`, `GattCharacteristic::GetDescriptorsAsync` | AVAILABLE / UNAVAILABLE |
| BLE read | `GattCharacteristic::ReadValueAsync` | AVAILABLE / UNAVAILABLE |
| BLE write (with/without response) | `GattCharacteristic::WriteValueAsync(IBuffer, GattWriteOption)` | AVAILABLE / UNAVAILABLE |
| BLE notify/indicate | `GattCharacteristic::ValueChanged` + `WriteClientCharacteristicConfigurationDescriptorAsync` | AVAILABLE / UNAVAILABLE |
| BLE pairing | `DeviceInformationPairing::PairAsync`, `Pairing.Custom.PairAsync` + `PairingRequested` | AVAILABLE / UNAVAILABLE (see pairing notes) |
| BLE RSSI (connected device) | **none exists in WinRT** | NOT_SUPPORTED, always; `ble_get_rssi` returns `RB_ERROR_NOT_SUPPORTED` |
| BLE reconnect | `GattSession.MaintainConnection` + worker-driven retry loop | AVAILABLE / UNAVAILABLE |
| Wi-Fi status | `NetworkInformation::GetInternetConnectionProfile` + `IsWlanConnectionProfile`/`ProfileName` | AVAILABLE / REQUIRES_PERMISSION / UNAVAILABLE (see gating) |
| Wi-Fi scan | `WiFiAdapter::RequestAccessAsync`, `FindAllAdaptersAsync`, `ScanAsync`, `NetworkReport.AvailableNetworks` | AVAILABLE / REQUIRES_PERMISSION / UNAVAILABLE |
| Wi-Fi connect | `WiFiAdapter::ConnectAsync` (2-arg open, 3-arg with `PasswordCredential`) | AVAILABLE / REQUIRES_PERMISSION / UNAVAILABLE |
| Wi-Fi disconnect | `WiFiAdapter::Disconnect` | AVAILABLE / REQUIRES_PERMISSION / UNAVAILABLE |
| Wi-Fi quality probe | **none exists in WinRT** | NOT_SUPPORTED, always |
| Wi-Fi saved networks | **none exists in WinRT** | NOT_SUPPORTED, always |

## Error mapping

### HRESULT -> `rb_status_t` (`map_hresult`)

| HRESULT | `rb_status_t` |
| --- | --- |
| `0` (S_OK) | `RB_OK` |
| `0x80070005` (E_ACCESSDENIED) | `RB_ERROR_PERMISSION_DENIED` |
| `0x80070490` / `0x80070491` (not found) | `RB_ERROR_NOT_FOUND` |
| `0x80070057` (E_INVALIDARG) | `RB_ERROR_INVALID_ARGUMENT` |
| `0x8007000E` (E_OUTOFMEMORY) | `RB_ERROR_NO_MEMORY` |
| `0x800704C7` (ERROR_CANCELLED) | `RB_ERROR_CANCELLED` |
| anything else | `RB_ERROR_INTERNAL` |

A null `BluetoothLEDevice` from `FromBluetoothAddressAsync` (the
normal "unknown address" outcome) maps to `RB_ERROR_NOT_FOUND`
asynchronously, per the public connect contract.

### `GattCommunicationStatus` -> `rb_status_t` (`map_gatt`)

| Status | `rb_status_t` |
| --- | --- |
| `Success` | `RB_OK` |
| `Unreachable` | `RB_ERROR_NOT_CONNECTED` |
| `ProtocolError` | `RB_ERROR_INTERNAL` |
| `AccessDenied` | `RB_ERROR_PERMISSION_DENIED` |

Note: on discovery, `AccessDenied` first triggers one basic
pairing attempt and a single retry; only a second denial becomes
`RB_ERROR_AUTH_FAILED` via the pairing path (see below).

### `DevicePairingResultStatus` -> `rb_status_t`

Only `Paired` is branched on: `Paired` -> `RB_OK`, any other
status -> `RB_ERROR_AUTH_FAILED`. An `E_ACCESSDENIED` HRESULT
thrown by the pairing call itself maps to
`RB_ERROR_PERMISSION_DENIED` via the HRESULT table. Other
`DevicePairingResultStatus` members were not enumerated here on
purpose: their exact names were not double-checked (see below),
and the code does not need them — anything that is not `Paired`
is a failed pairing.

### `WiFiConnectionStatus` -> `rb_status_t`

| Status | `rb_status_t` |
| --- | --- |
| `Success` | `RB_OK` |
| `AccessRevoked` | `RB_ERROR_PERMISSION_DENIED` |
| `InvalidCredential` | `RB_ERROR_AUTH_FAILED` |
| `NetworkNotAvailable` | `RB_ERROR_NOT_FOUND` |
| `Timeout` | `RB_ERROR_TIMEOUT` |
| `UnsupportedAuthenticationProtocol` | `RB_ERROR_NOT_SUPPORTED` |
| `UnspecifiedFailure` | `RB_ERROR_INTERNAL` |

### `WiFiAccessStatus` (probe / pre-flight)

`Allowed` proceeds. `DeniedBySystem` (documented: returned whenever
the app lacks the `wifiControl` capability) and `DeniedByUser`
both make Wi-Fi capabilities `REQUIRES_PERMISSION` and make scan /
connect return `RB_ERROR_PERMISSION_DENIED`.

## Documented limitations (read before shipping)

1. **Connected-device RSSI does not exist in WinRT.** Scan results
   carry real RSSI from advertisements. `rb_ble_get_rssi()` is
   NOT_SUPPORTED by design, never a fabricated number.
2. **Disconnect is a request, not a synchronous teardown.** WinRT
   has no `Disconnect` method; closing the device and service
   handles drops the link after a small OS timeout and only if no
   other app holds the device. `rb_ble_disconnect()` returns once
   teardown is requested; the general `RB_EVENT_BACKEND_STATE`
   event reports the change. Connection callbacks are NOT fired
   for an explicit disconnect (public contract).
3. **Connection attempts and GATT requests cannot be cancelled.**
   Documented in Microsoft's GATT client guide ("Currently, you
   can't cancel the connection process"). Scans stop promptly via
   `watcher.Stop()`. A connect `timeout_ms` is therefore accepted
   but NOT enforced on the attempt itself; a disconnect requested
   mid-attempt is honored as soon as the attempt resolves.
   GATT requests queue behind each other (~7 s per queued
   request, per the same guide); the synchronous-call timeouts
   (discover 120 s, read/write/subscribe/unsubscribe 60 s, Wi-Fi
   SSID check 30 s, probe/create 15 s, Wi-Fi disconnect/check
   10 s) are backend-chosen caps, not spec values.
4. **Wi-Fi is capability-gated, and packaging decides the
   outcome.** Learn documents that without the `wifiControl`
   device capability, `RequestAccessAsync` always returns
   `DeniedBySystem` and adapter enumeration fails. A packaged
   (MSIX / sparse-package) consumer that declares `wifiControl`
   gets full Wi-Fi; an unpackaged Win32 desktop app has no
   manifest to declare it in and will therefore see
   REQUIRES_PERMISSION / PERMISSION_DENIED. The exact unpackaged
   behaviour needs on-device confirmation — Microsoft's wording
   covers the packaged case.
5. **Location consent gates BSSID access.** Without it, scanned
   networks may report an empty BSSID. The backend reports what
   the API returns and never invents one.
6. **Wi-Fi security is unknown by construction.**
   `WiFiAvailableNetwork` exposes no authentication information,
   so every scanned network reports `RB_WIFI_SEC_UNKNOWN`.
7. **No hidden-network guessing.** Unknown SSIDs report
   NOT_FOUND synchronously (public contract), so the hidden-
   network `ConnectAsync` overload is not used.
8. **Wi-Fi status has no IP/gateway.** The WinRT Wi-Fi surface
   exposes neither for the connection; those fields stay empty
   with an explanatory message.
9. **Service attribute handles are 0.** WinRT exposes
   `AttributeHandle` on characteristics and descriptors but not
   on services, so `rb_ble_service_t.handle` is always 0.
10. **Reconnect policy semantics.** Reconnection is active when
    the stored policy is enabled (the default) OR the connect
    call sets `auto_reconnect`. Per-connection
    `max_reconnect_attempts` / `reconnect_delay_ms` override the
    policy when non-zero; a policy `max_attempts` of 0 means
    unlimited while the context lives. The worker, not the OS
    alone, drives retries with backoff (initial delay x 2 per
    attempt by default, capped at 30 s).
11. **Pairing reachability.** The public vtable has no pairing
    slot. Basic pairing runs only as a fallback when uncached
    discovery fails with `AccessDenied`. Custom pairing
    (ConfirmOnly auto-accept, ProvidePin with a caller-supplied
    PIN) is implemented in `rb_windows.cpp` for a future public
    pairing entry point; `DisplayPin` / `ConfirmPinMatch` return
    NOT_SUPPORTED because a headless library cannot show a PIN
    to a user, and no PIN is ever logged. Unanswered ceremonies
    are left to fail rather than silently confirmed.
12. **TX power comes from GAP data sections, not from the
    `TransmitPowerLevelInDBm` event-args property.** The section
    encoding (type `0x0A`, one signed byte) is the documented GAP
    layout; the property's exact nullability was not
    double-checked, so it is not used.
13. **Shutdown race (residual).** `rb_win_destroy()` signals every
    stop/disconnect event but does not join detached connect
    workers blocked in uncancellable WinRT calls. All callbacks
    and core touches are gated on the `ctx_live` flag, so a worker
    that resolves after destroy exits quietly — but a callback
    already in flight at the exact moment `rb_shutdown()` frees
    the context is a residual race shared with any detached-
    thread design. If strict shutdown safety is required, the
    integrator should retain thread handles and join them (or
    refcount the context) rather than detaching.
14. **Threading exception, stated plainly.** All threads come from
    `rb_thread.h`. The shim has no condition variable or event
    primitive, so Win32 `HANDLE` events are used ONLY to signal
    worker completion / stop — never to create threads. WinRT
    objects are agile, so MTA worker threads and WinRT pool
    threads may all touch them; every worker thread initializes
    itself as MTA via `ensure_apartment()`.

## First-build verification checklist

Compile-time (Windows SDK machine, first thing):

- [ ] `winrt::guid` aggregate layout (`Data1`/`Data2`/`Data3`/`Data4`)
      as used by `guid_from_rb` / `rb_from_guid`.
- [ ] C++/WinRT event-token removal via the same-name call
      (`watcher.Received(token)`, `device.ConnectionStatusChanged(token)`,
      `characteristic.ValueChanged(token)`,
      `custom.PairingRequested(token)`).
- [ ] `BluetoothAdapter::GetDefaultAsync()` exists with that name.
- [ ] `GattDeviceService::Close()` (IClosable) and
      `GattDeviceService::Session()`.
- [ ] `GattCharacteristic::GetDescriptorsAsync()`,
      `GattDescriptor::Uuid()` / `AttributeHandle()`, and that
      `AttributeHandle` is 16-bit.
- [ ] `GattCharacteristic::WriteValueAsync(IBuffer, GattWriteOption)`
      and `GattWriteOption::{WriteWithResponse, WriteWithoutResponse}`.
- [ ] CCCD values `Indicate` / `None` (only `Notify` was verified
      against the GATT guide).
- [ ] `GattCommunicationStatus::{ProtocolError, AccessDenied}`
      member names.
- [ ] `BluetoothCacheMode::Uncached` member name.
- [ ] `WiFiAvailableNetwork::NetworkRssiInDecibelMilliwatts`
      nullability (`IReference<int32>` assumed) and
      `ChannelCenterFrequencyInKilohertz` type.
- [ ] `WiFiAccessStatus::DeniedByUser` member name.
- [ ] `PasswordCredential` default constructor + `Password` setter.
- [ ] Connectivity members: `NetworkInformation::
      GetInternetConnectionProfile()`,
      `ConnectionProfile::IsWlanConnectionProfile()`,
      `ConnectionProfile::ProfileName()`.
- [ ] `BluetoothLEAdvertisementType::{ConnectableUndirected,
      ConnectableDirected}` member names.
- [ ] `BluetoothLEAdvertisement::DataSections()` /
      `DataSection::DataType()` / `Data()` for TX-power parsing.
- [ ] `DevicePairingRequestedEventArgs::Accept(String)` overload
      (bare `Accept()` is verified; the PIN overload is not).
- [ ] `windowsapp.lib` link (or the CppWinRT NuGet package) plus
      `/std:c++17`, and that the C file still compiles as C11
      under MSVC.

On-device (Bluetooth + Wi-Fi hardware, packaged AND unpackaged):

- [ ] Probe reports adapter presence correctly, including
      radio-off and no-adapter machines.
- [ ] Scan delivers advertisements with RSSI, names, service
      UUIDs, manufacturer data; duplicate filtering honours
      `allow_duplicates`.
- [ ] Connect -> discover -> read/write/notify round-trips
      against a real peripheral; AccessDenied pairing fallback
      against a bonding-required peripheral.
- [ ] Disconnect actually drops the link when this app is the
      only holder; link drop from the peripheral side fires
      DISCONNECTED and the reconnect policy retries.
- [ ] Wi-Fi scan/connect on a packaged app with `wifiControl`;
      unpackaged behaviour (expected: REQUIRES_PERMISSION) and
      location-consent effects on BSSID.
- [ ] `rb_shutdown()` with an in-flight connect does not crash
      (see limitation 13).

## Sources verified against (Microsoft Learn unless noted)

- GATT client guide (connection semantics, ~7 s queueing, no
  cancel, MaintainConnection, CCCD Notify, ReadValueAsync):
  https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/gatt-client
- BLE advertisements (watcher, Active scanning, manufacturer
  filter):
  https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/ble-beacon
- `BluetoothLEDevice` class (FromBluetoothAddressAsync/FromIdAsync,
  ConnectionStatusChanged, GetGattServicesAsync overloads,
  GetGattServicesForUuidAsync, Close, DeviceInformation):
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice?view=winrt-26100
- `BluetoothLEDevice.DeviceInformation` property:
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetoothledevice.deviceinformation?view=winrt-26100
- Advertisement namespace (watcher / advertisement / manufacturer
  data classes):
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement?view=winrt-22621
- Watcher `ScanningMode` (bluetooth capability note):
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementwatcher.scanningmode?view=winrt-22621
- `GattSession.MaintainConnection`:
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattsession.maintainconnection?view=winrt-26100
- `GattCharacteristicProperties` values (Broadcast 1, Read 2,
  WriteWithoutResponse 4, Write 8, Notify 16, Indicate 32):
  https://learn.microsoft.com/nb-no/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattcharacteristicproperties?view=winrt-15063
- Pairing guide (basic pairing, IsPaired/CanPair):
  https://learn.microsoft.com/vi-vn/windows/uwp/devices-sensors/pair-devices
- Custom pairing, C++/WinRT sample (PairAsync(ceremonies),
  PairingRequested handler, Accept()):
  https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/pairing-a-set
- `DevicePairingKinds` (ConfirmOnly/DisplayPin/ProvidePin/
  ConfirmPinMatch/ProvidePasswordCredential):
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.enumeration.devicepairingkinds?view=winrt-19041
- `DeviceInformationPairing.Custom`:
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.enumeration.deviceinformationpairing.custom?view=winrt-26100
- `WiFiAdapter` class (RequestAccessAsync DeniedBySystem note,
  FindAllAdaptersAsync/FromIdAsync capability notes, ScanAsync,
  NetworkReport, ConnectAsync overloads, Disconnect,
  AvailableNetworksChanged):
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.wifi.wifiadapter?view=winrt-22621
- `WiFiAvailableNetwork` (Bssid, ChannelCenterFrequencyInKilohertz,
  NetworkRssiInDecibelMilliwatts, location-consent notes):
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.wifi.wifiavailablenetwork?view=winrt-26100
- `WiFiConnectionStatus` values:
  https://learn.microsoft.com/en-us/uwp/api/windows.devices.wifi.wificonnectionstatus?view=winrt-26100
- `WiFiAdapter.RequestAccessAsync` (DeniedBySystem without
  wifiControl):
  https://learn.microsoft.com/zh-cn/uwp/api/windows.devices.wifi.wifiadapter.requestaccessasync?view=winrt-28000
- Wi-Fi scanner sample (RequestAccessAsync, FindAllAdaptersAsync
  via device selector, ScanAsync, NetworkReport):
  https://learn.microsoft.com/en-us/archive/msdn-magazine/2016/july/modern-apps-build-a-wi-fi-scanner-in-the-uwp
- Calling WinRT from desktop apps (C++/WinRT, C++17, package
  identity note):
  https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/desktop-to-uwp-enhance

## APIs relied on that were NOT double-checked against Learn

Honest list — each of these must be confirmed at first build
(the checklist above covers them). Nothing else in the code
depends on an unverified member:

- `BluetoothAdapter::GetDefaultAsync()`.
- `BluetoothLEAdvertisementType::{ConnectableUndirected,
  ConnectableDirected}`.
- `BluetoothLEAdvertisement::DataSections()` /
  `BluetoothLEAdvertisementDataSection::DataType()` / `Data()`.
- `GattDeviceService::Session()` (only seen in secondary samples)
  and `GattDeviceService::Close()`.
- `GattCharacteristic::GetDescriptorsAsync()`,
  `GattDescriptor::Uuid()` / `AttributeHandle()`, and the 16-bit
  width of `AttributeHandle`.
- `GattCharacteristic::WriteValueAsync(IBuffer, GattWriteOption)`
  and `GattWriteOption::{WriteWithResponse, WriteWithoutResponse}`
  (only the single-argument `WriteValueAsync(IBuffer)` was seen
  in the GATT guide).
- `GattClientCharacteristicConfigurationDescriptorValue::{Indicate,
  None}` (only `Notify` was seen in the guide).
- `GattCommunicationStatus::{ProtocolError, AccessDenied}` (only
  `Success` and unreachable behaviour were seen).
- `DevicePairingResultStatus` members other than `Paired` (the
  code branches only on `Paired`).
- `DevicePairingRequestedEventArgs::Accept(String)` (bare
  `Accept()` is verified).
- `BluetoothCacheMode::Uncached` (`Cached` was seen; `Uncached`
  is its symmetric partner).
- `WiFiAvailableNetwork::NetworkRssiInDecibelMilliwatts`
  nullability and `ChannelCenterFrequencyInKilohertz` type.
- `WiFiAccessStatus::DeniedByUser` (`Allowed` and `DeniedBySystem`
  are verified).
- `PasswordCredential` default constructor + `Password` setter.
- Connectivity: `NetworkInformation::
  GetInternetConnectionProfile()`,
  `ConnectionProfile::IsWlanConnectionProfile()`,
  `ConnectionProfile::ProfileName()`.
- C++/WinRT event-token removal via the same-name call.
- `winrt::guid` aggregate member layout.
- Unpackaged-desktop Wi-Fi behaviour (inferred from the packaged
  wording, not directly documented).
