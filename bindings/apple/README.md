# Apple bindings — Phase 3 skeleton (not built)

Status: documented skeleton only. There is no Xcode and no Apple SDK
on the Phase 1 build machine, so nothing here compiles yet.

## Requirements for the real binding

- Xcode + an Apple device or Mac (simulators cannot do Bluetooth).
- Core Bluetooth (`CBCentralManager` / `CBPeripheral`) for BLE central
  role; Network framework (`NWPathMonitor`) for connectivity state;
  `NEHotspotConfiguration` for Wi-Fi connection requests where the
  platform permits them.

## Rules the binding must follow

- Delegate callbacks arrive on the queue the central manager was
  created with (main queue by default). The binding forwards them to
  RadioBridge callbacks without blocking that queue.
- `CBPeripheral`, `CBService`, `CBCharacteristic` objects stay inside
  the adapter. The C API only ever sees `rb_ble_device_t` and GATT
  structs. Apple identifies peripherals by UUID, not MAC address: the
  binding exposes that UUID string as the public `address` field and
  documents the difference.
- RSSI: Core Bluetooth supports `readRSSI`, so the Apple backend can
  report `RB_CAP_AVAILABLE` for RSSI (unlike WinRT).
- Wi-Fi: modern iOS restricts current-network info
  (`CNCopyCurrentNetworkInfo` needs entitlement + location) and
  connection requests go through `NEHotspotConfiguration`. The
  backend must report those restrictions through capabilities and
  diagnostics, never fabricate SSIDs.
- Info.plist usage descriptions (`NSBluetoothAlwaysUsageDescription`,
  `NSLocationWhenInUseUsageDescription` where scanning needs it) are
  the app's job; the library only reports what is required.

## Files

- `objc/RadioBridge.m` — Objective-C wrapper sketch (not compiled).
- `swift/RadioBridge.swift` — Swift wrapper sketch (not compiled).
