# Permission management

The library **never requests permissions itself**. It reports what an
operation requires on the active backend
(`rb_get_required_permissions()`), and the application drives the
OS-managed flow. This is deliberate: permission UX belongs to the app,
and a library that pops system dialogs is a liability.

## Per-operation requirements (mock baseline)

| Operation | Reported permissions |
| --------- | -------------------- |
| BLE scan | `BLUETOOTH_SCAN` + `LOCATION` |
| BLE connect / read / write / notify | `BLUETOOTH_CONNECT` |
| Wi-Fi scan | `WIFI_SCAN` + `LOCATION` |
| Wi-Fi connect | `WIFI_CONNECT` |
| Wi-Fi status / quality | none |

## Platform notes

- **Android**: API 31+ splits Bluetooth into `BLUETOOTH_SCAN`,
  `BLUETOOTH_CONNECT`, `BLUETOOTH_ADVERTISE` (runtime permissions);
  pre-31 scanning is location-gated (`ACCESS_FINE_LOCATION`).
  `BLUETOOTH_ADMIN` is legacy. Wi-Fi needs `ACCESS_WIFI_STATE` /
  `CHANGE_WIFI_STATE`; scanning is throttled and location-gated on
  modern versions.
- **Apple**: `NSBluetoothAlwaysUsageDescription` for Bluetooth;
  location usage descriptions where scanning needs them; Wi-Fi
  current-network info needs entitlement + location on modern iOS.
- **Windows**: the app declares the `bluetooth` capability; device
  access needs an explicit permission request before connection.
- **Linux**: BlueZ/NetworkManager access is mediated by the system bus
  policy and PolicyKit; a denied call surfaces as
  `RB_ERROR_PERMISSION_DENIED`, never as a silent empty result.

## Secrets

- Wi-Fi passwords are passed to `rb_wifi_connect()` and used only for
  the attempt. They are never logged, never stored, never included in
  diagnostics. `test_logging` proves this with a distinctive password
  and a log capture.
- Pairing secrets, like passwords, must never appear in logs or error
  messages on any backend. Backends must scrub native error strings
  that could embed credentials before forwarding them.
- Advertisement data and SSIDs are not secrets, but device addresses
  are identifiers: logs should prefer the library's own diagnostic
  level (INFO and below carry no addresses by default in the mock
  backend; native backends must follow the same rule).
