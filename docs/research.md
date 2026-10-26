# Research grounding

The Phase 1 API shape, capability model, error taxonomy, and backend
structure were grounded in web research into the real platform APIs,
not designed from memory. Key sources consulted:

> Scope note (2026-10-26): Apple is out of scope — Apple sources
> below are retained for reference only; no Apple backend or
> bindings exist in this project.

## Linux — BlueZ (D-Bus)

- Python/GIO BlueZ walkthrough (service `org.bluez`, Adapter1/Device1
  object paths, GATT hierarchy):
  https://ukbaz.github.io/howto/python_gio_1.html
- BlueZ GattCharacteristic D-Bus reference (ReadValue/WriteValue,
  StartNotify/StopNotify, Value property, error names):
  https://man.archlinux.org/man/extra/bluez-utils/org.bluez.GattCharacteristic.5.en
  and https://www.mankier.com/5/org.bluez.GattCharacteristic
- BlueZ D-Bus integration notes (bless project):
  https://deepwiki.com/kevincar/bless/3.1.1-dbus-integration
- BlueZ core architecture (bluez.dart project):
  https://deepwiki.com/canonical/bluez.dart/3-core-architecture

## Linux — NetworkManager (D-Bus)

- Client interfaces (GetDevices, ActivateConnection,
  AddAndActivateConnection, DeactivateConnection, Device.Wireless
  RequestScan/GetAccessPoints):
  https://deepwiki.com/NetworkManager/NetworkManager/4-client-interfaces
- NetworkManager settings D-Bus API (ListConnections, GetSettings,
  Update, Delete):
  https://networkmanager.dev/docs/api/1.40/nm-settings-dbus.html
- Wi-Fi discovery notes (cosmic-settings-subscriptions project):
  https://deepwiki.com/pop-os/cosmic-settings-subscriptions/3.1-wifi-and-device-discovery

## Android

- BluetoothGatt reference (connectGatt, discoverServices,
  readCharacteristic, callbacks):
  https://developer.android.com/reference/android/bluetooth/BluetoothGatt
- BluetoothLeScanner source (scan error codes
  SCAN_FAILED_ALREADY_STARTED / INTERNAL_ERROR / FEATURE_UNSUPPORTED /
  OUT_OF_HARDWARE_RESOURCES):
  https://www.javatips.net/api/platform_frameworks_base-master/core/java/android/bluetooth/le/BluetoothLeScanner.java
- BluetoothGatt lifecycle (binder-thread callbacks, marshaling
  requirement):
  https://dev.to/ble_advertiser/mastering-bluetoothgatt-lifecycle-preventing-memory-leaks-and-stale-connections-in-android-1036

## Apple

- Core Bluetooth platform implementations (CBCentralManager /
  CBPeripheral delegate model, discoverServices/discoverCharacteristics,
  didUpdateValueFor, readRSSI):
  https://deepwiki.com/inthehand/32feet/3.3-ble-platform-implementations
- Core Bluetooth tutorial (scanForPeripherals, didDiscover,
  advertisementData + RSSI, connect, discoverServices):
  https://www.kodeco.com/336-core-bluetooth-in-watchos-tutorial
- BLE peripheral implementation notes (iOS/macOS):
  https://deepwiki.com/rohitsangwan01/ble_peripheral/5.2-iosmacos-implementation

## Windows (WinRT)

- GATT client reference (BluetoothLEAdvertisementWatcher,
  BluetoothLEDevice.FromBluetoothAddressAsync, GetGattServicesAsync /
  GetCharacteristicsAsync, GattSession.MaintainConnection):
  https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/gatt-client
- GattCharacteristicProperties flags (Read 2, Write 8,
  WriteWithoutResponse 4, Notify 16, Indicate 32):
  https://learn.microsoft.com/nb-no/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattcharacteristicproperties?view=winrt-15063
- Documented WinRT limits (no explicit disconnect, no RSSI read, no
  MTU request, ~7s per queued request, non-cancellable connect):
  https://deepwiki.com/inthehand/32feet/3.3-ble-platform-implementations

## Cross-platform libraries (API-shape lessons)

- SimpleBLE (layered Core API + platform backends + language
  bindings; Linux needs separate D-Bus and BlueZ layers; per-platform
  error mapping; hardware-gated integration tests):
  https://simpleble.org
  https://deepwiki.com/simpleble/simpleble/1-overview
  https://deepwiki.com/simpleble/simpleble/2-api-documentation
  https://deepwiki.com/simpleble/simpleble/3.4-android-backend
  https://deepwiki.com/simpleble/simpleble/4-build-system

## What the research changed in the design

- The capability model exists because no two platforms expose the
  same set (WinRT cannot read RSSI or disconnect explicitly; Apple
  has no MAC addresses; Android scanning is permission- and
  throttle-gated).
- The error taxonomy carries `ALREADY_IN_PROGRESS` and `BUSY`
  separately because Android distinguishes "already started" from
  "out of hardware resources".
- Cancellation is documented as best-effort per platform because
  WinRT cannot cancel an in-flight connection attempt.
- Callbacks are specified as worker-thread deliveries because Android
  binder threads and Core Bluetooth queues both deliver off the
  caller's thread.
- The Linux backend probes before claiming anything, because both
  BlueZ and NetworkManager are daemons that may be absent.
- RadioBridge differs from SimpleBLE by design: a C11 stable C ABI
  (SimpleBLE's core is C++), with the same layered-backend lesson.
