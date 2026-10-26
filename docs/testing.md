# Testing

## Running

```sh
cmake --preset default
cmake --build build -j1
ctest --test-dir build --output-on-failure
```

12 CTest entries, all passing on the Phase 1 build machine (no radios,
no SDKs). Total runtime ~20s; the BLE scan test dominates (it runs
eight scans to cover every filter).

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
hardware is absent. The Phase 1 suite runs entirely on mocks, so it
passes everywhere. Phase 2 adds a hardware-gated test that:

1. Probes for a real adapter (BlueZ adapter dir + daemon).
2. Skips with a clear message when absent.
3. Runs real scan/connect/GATT checks only when present.

No test in this repo may invent a device, a network, or a connection
to make a suite green.
