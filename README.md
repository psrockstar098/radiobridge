# RadioBridge

A cross-platform C library that unifies Bluetooth and Wi-Fi behind
one developer API, with native bindings for Android, Apple, Windows,
and Linux. One API, one capability model, one error taxonomy — the
platform differences stay inside the backends, never in your code.

**Version 0.1.0 — Phase 1 (Foundation).**

## What works today (proven, tested)

- The full public C11 API: `rb_initialize`, `rb_get_capabilities`,
  `rb_ble_scan_start`, `rb_ble_connect`, `rb_wifi_get_status`,
  `rb_cancel`, `rb_shutdown`, plus the supporting surface — GATT
  discover/read/write/notify, RSSI, reconnect policy, Wi-Fi scan,
  Wi-Fi connect/disconnect, network-change events, quality
  measurement, structured logging, permission reporting.
- The capability model: every backend reports exactly what it can do
  (`AVAILABLE` / `UNAVAILABLE` / `REQUIRES_PERMISSION` /
  `NOT_SUPPORTED`). Nothing is promised identically everywhere.
- The async event system: scan results, connection changes, data,
  and errors arrive via callbacks on internal threads — never
  blocking the caller.
- The mock Bluetooth backend — fully working, deterministic, no
  radio needed: 3 fake BLE devices with advertised metadata,
  service-UUID / manufacturer / RSSI / name filtering, connect /
  disconnect / reconnect with policy, GATT service/characteristic/
  descriptor discovery, characteristic read/write, notifications,
  RSSI diagnostics, simulated drops.
- The mock Wi-Fi backend — fully working: fake Wi-Fi state, network
  discovery (3 networks), connection requests, SSID/BSSID/IP
  metadata, network-change events, connectivity diagnostics
  (disconnected / unavailable / permission-denied / failed), and
  simulated quality measurement against a fake endpoint.
- The Linux backend structure: real BlueZ D-Bus and NetworkManager
  interface definitions, plus an honest probe that reports
  `UNAVAILABLE` with a clean error when the platform service is
  absent. It fabricates nothing.
- 12 CTest tests, all passing, covering lifecycle, capabilities,
  mock BLE scan/filter/connect/GATT/notify, mock Wi-Fi
  status/scan/connect, cancellation, error paths, callback
  threading, logging (including "passwords are never logged"),
  permissions, the Linux backend contract, and mock hooks.
- Two C examples (`examples/c/ble_scan.c`, `examples/c/wifi_status.c`).

## What is honestly unavailable, and why

- **Real radio backends on this build machine**: the machine has no
  Bluetooth hardware, no Wi-Fi tools, no D-Bus system bus, and no
  platform SDKs (no Android SDK, no JDK, no Xcode, no Windows SDK).
  Real scan results or connections cannot be produced here, and the
  code refuses to fake them.
- **Linux D-Bus transport** (Phase 2): the interface definitions are
  written and compiled in, but the transport (libdbus) is not linked
  yet. On a machine with bluetoothd + NetworkManager running, the
  backend still reports `UNAVAILABLE` until that transport lands.
- **Android / Apple / Windows backends** (Phase 3): documented stubs
  only, with skeleton bindings in `bindings/`. They report
  `NOT_SUPPORTED` and fail cleanly.
- **Pairing and saved networks**: no API for them exists in Phase 1;
  they are reported `NOT_SUPPORTED`, not "temporarily missing".

## Build, run, test

```sh
cmake --preset default        # Unix Makefiles, Release
cmake --build build -j1       # -j1: the build machine is RAM-tight
ctest --test-dir build --output-on-failure

./build/examples/example_ble_scan
./build/examples/example_wifi_status
```

Requirements: C11 compiler, CMake 3.16+, pthreads. No other
dependencies in Phase 1.

## Real-hardware try-out (when you have a machine with radios)

1. Build and run the mock examples first — they prove the API shape
   without hardware.
2. For Linux Phase 2: install `bluez` + `NetworkManager` + libdbus
   dev headers, make sure `bluetoothd` and NetworkManager are
   running, then follow the Phase 2 checklist in
   `docs/backends.md`.
3. Hardware tests are gated: they skip, never fake, when no adapter
   is present (see `docs/testing.md`).

## Roadmap

- **Phase 1 — Foundation (done)**: C11 API, capability model, async
  events, permission reporting, mock backends, Linux structure,
  documented stubs, CTest suite, docs.
- **Phase 2 — Linux transport**: real BlueZ + NetworkManager D-Bus
  implementation, hardware-gated integration tests, install targets
  and packaging.
- **Phase 3 — Bindings**: real JNI/Java/Kotlin and ObjC/Swift
  bindings, then the Android/Apple/Windows backends behind them.
- **Later**: pairing, saved networks, advanced features (mesh, BLE
  advertising, Wi-Fi Direct, coexistence) — only when the platform
  APIs support them honestly.

## Docs

- `docs/api.md` — API contract (ABI rules, ownership, threading,
  cancellation, errors)
- `docs/capabilities.md` — capability model + per-backend matrix +
  documented platform limits
- `docs/backends.md` — backend architecture, D-Bus definitions,
  error mapping, Phase 2 checklist
- `docs/permissions.md` — per-operation permission tables, the
  never-request rule, secret handling
- `docs/testing.md` — suite layout, mock catalog, hardware-gating
  policy
- `docs/research.md` — sources the design is grounded in

## License

To be decided by the project owner (no LICENSE file yet, by design).
