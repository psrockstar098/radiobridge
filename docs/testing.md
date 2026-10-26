# Testing

## Running

```sh
cmake --preset default
cmake --build build -j1
ctest --test-dir build --output-on-failure
```

16 CTest entries, all passing on the build machine (no radios, no
SDKs). Total runtime ~60s; the conformance suite dominates (two
10-second scans per backend).

## Suite layout

| Test | Covers |
| ---- | ------ |
| `test_lifecycle` | init/shutdown, defaults, bad backend kind, version |
| `test_capabilities` | mock capability surface, linux probe honesty, stub failures |
| `test_ble_scan` | full catalog, service/manufacturer/RSSI/name filters, timeout, concurrent-scan rejection, stop |
| `test_ble_gatt` | connect (incl. async unknown-device failure), discover, read/write, notify subscribe/unsubscribe, RSSI, disconnect paths |
| `test_wifi` | status, scan, connect (open/secured/unknown/auth), quality, disconnect, change events |
| `test_cancel` | scan cancel freezes deliveries, connect cancel delivers CANCELLED, bogus ids |
| `test_errors` | NULL-context and bad-argument paths, UUID parsing, status strings |
| `test_threading` | callbacks arrive off the caller thread, concurrent contexts, concurrent capability reads |
| `test_logging` | level filtering, callback delivery, password never logged |
| `test_permissions` | masks + names per operation, count query, invalid inputs |
| `test_linux_backend` | probe detail paths, all-UNAVAILABLE contract, explicit init failure |
| `test_mock_hooks` | simulated drops, reconnect policy (enabled + disabled) |
| `test_app` | Phase 5: registry JSON round-trip, lossy-link reliable transfer (drop/dup/corrupt), firmware chunks + corrupt rejection, OpenSSL secure channels (round-trip, tamper, replay, wrong-key), multi-device sessions, scheduler, power config, policy ranking |
| `test_linux_bus` | Phase 2: fake BlueZ + NetworkManager service on a private bus — caps, diagnostics, BLE scan filters, connect/discover/read/write/notify/RSSI/disconnect, Wi-Fi status/scan/connect/disconnect, quality probes |
| `test_conformance` | Phase 5: same scripted sequence against mock + Linux fake-bus backends (shapes, not raw counts; documented divergences asserted per backend) |
| `test_jni_smoke` | Phase 3: JVM loads the built JNI library, drives the mock backend end to end (scan/connect/read) |

## Fake-bus verification (Phase 2, no hardware needed)

`tests/fake_bus/fake_bus.c` owns `org.bluez` and
`org.freedesktop.NetworkManager` on a private `dbus-daemon` bus
(address from `RB_DBUS_ADDRESS`) and exposes the real interfaces
with canned devices (adapter `00:11:22:33:44:55`, heart + thermo
devices with GATT services) and networks (open + WPA2 APs, IP
config). `test_linux_bus` drives the backend against it over the
real D-Bus protocol. The fake service is a test tool, not part of
the library, and its data is labeled fake everywhere. A run
against real radios on real hardware is still the remaining
verification step — the fake-bus run does not claim to replace it.

## Mock catalog (deterministic, fake by design)

- BLE: `AA:BB:CC:00:00:01` "RB Heart Rate" (-52 dBm, services
  0x180D/0x180F), `AA:BB:CC:00:00:02` "RB Thermo Beacon" (-68 dBm,
  service 0x1809, manufacturer 0x004C), `AA:BB:CC:00:00:03`
  "RB Demo Tag" (-81 dBm, custom 128-bit service + echo char).
- Wi-Fi: `RB-HomeNet` (WPA2, -45 dBm, 5 GHz), `RB-CoffeeShop`
  (open, -62 dBm, 2.4 GHz), `RB-Lab-5G` (WPA3, -58 dBm, 5 GHz).
- Mock quality numbers are simulated from signal strength and are
  labeled simulated in code, docs, and example output.

## Hardware gating policy (SimpleBLE lesson)

Integration and hardware tests must **skip, never fake**, when the
hardware is absent. The suite runs on mocks and the fake bus, so it
passes everywhere without radios. A future hardware-gated test
must:

1. Probes for a real adapter (BlueZ adapter dir + daemon).
2. Skips with a clear message when absent.
3. Runs real scan/connect/GATT checks only when present.

No test in this repo may invent a device, a network, or a connection
to make a suite green.
