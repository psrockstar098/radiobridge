# Capability model

The core rule: **never promise identical radio capabilities
everywhere.** The shared API standardizes the developer experience;
each backend reports exactly what it can do, and callers check
`rb_get_capabilities()` before calling an operation.

## States

| State | Meaning |
| ----- | ------- |
| `RB_CAP_AVAILABLE` | feature works on this backend |
| `RB_CAP_UNAVAILABLE` | backend present but feature missing right now (no adapter, service down, transport not yet implemented) |
| `RB_CAP_REQUIRES_PERMISSION` | feature gated on an OS permission |
| `RB_CAP_NOT_SUPPORTED` | backend can never do this (platform limit or no API in this phase) |

## Capability matrix (verified)

Linux "live" column = services present (fake-bus verified by
`test_linux_bus`); without services every Linux feature is
`UNAVAILABLE` (covered by `test_linux_backend`). Windows column
is the backend's design intent from documented WinRT APIs —
**compile-unverified**, not a test result. Android has a compiled
JNI binding but no native backend yet, so it stays `NOT_SUPPORTED`.

| Feature | mock | linux (live) | linux (no services) | windows (design) | android stub |
| ------- | ---- | ------------ | ------------------- | ---------------- | ------------ |
| BLE scan | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| BLE connect | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| BLE GATT | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| BLE notify | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| BLE pairing | NOT_SUPPORTED | AVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| BLE RSSI | AVAILABLE | AVAILABLE | UNAVAILABLE | NOT_SUPPORTED | NOT_SUPPORTED |
| BLE reconnect | AVAILABLE | UNAVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| Wi-Fi status | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE | NOT_SUPPORTED |
| Wi-Fi scan | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE / REQUIRES_PERMISSION | NOT_SUPPORTED |
| Wi-Fi connect | AVAILABLE | AVAILABLE | UNAVAILABLE | AVAILABLE / REQUIRES_PERMISSION | NOT_SUPPORTED |
| Wi-Fi quality | AVAILABLE | AVAILABLE | UNAVAILABLE | NOT_SUPPORTED | NOT_SUPPORTED |
| Wi-Fi saved networks | NOT_SUPPORTED | NOT_SUPPORTED | UNAVAILABLE | NOT_SUPPORTED | NOT_SUPPORTED |

Notes:

- Apple is out of scope since 2026-10-26: no Apple backend or bindings exist; the `RB_BACKEND_APPLE` enum value is retained only for ABI stability and always reports `NOT_SUPPORTED`.

- Mock pairing/saved-networks are `NOT_SUPPORTED`, not `UNAVAILABLE`:
  there is no pairing or saved-network function in the API surface
  at all for the mock, so claiming them "temporarily missing" would
  be wrong.
- Linux without services is `UNAVAILABLE` across the board: the
  probe finds no adapter, no D-Bus system bus, and no
  NetworkManager on the build VM. With services present (real or
  fake-bus), the matrix above is what the backend reports.
- Windows Wi-Fi shows two states because the `wifiControl`
  manifest capability gates it: a packaged consumer declaring
  `wifiControl` gets full Wi-Fi; without it, `RequestAccessAsync`
  returns `DeniedBySystem` and the backend reports
  `REQUIRES_PERMISSION` / `PERMISSION_DENIED`. Exact unpackaged
  behavior needs on-device confirmation.
- Stubs are `NOT_SUPPORTED` because their SDKs cannot run here at
  all — a different, stronger statement than `UNAVAILABLE`.

## Documented platform limits (grounded in research, see research.md)

- **WinRT**: no explicit disconnect, no RSSI read, no MTU request,
  connection attempts not cancellable, ~7s per queued GATT request.
  The Windows backend will report RSSI as `NOT_SUPPORTED` permanently.
- **Android**: Wi-Fi scanning is throttled and location-gated on
  modern versions; BLE scanning needs `BLUETOOTH_SCAN` (31+) or
  location (pre-31). Callbacks arrive on binder threads.
  Wi-Fi current-network info is entitlement-gated; connection
  requests go through `NEHotspotConfiguration`.
- **Linux/BlueZ**: GATT client paths are per-device object paths;
  pairing needs an agent; adapter power/scan state is explicit.
- **NetworkManager**: scanning (`RequestScan`) and connection
  (`ActivateConnection`) are separate operations with separate
  permission requirements; saved connections live in Settings.
