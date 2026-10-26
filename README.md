# RadioBridge

A cross-platform C library that unifies Bluetooth and Wi-Fi behind
one developer API, with native backends for Linux (BlueZ +
NetworkManager over D-Bus) and Windows (WinRT, compile-verified by
CI, runtime-unverified), a JNI binding for Android, and one
capability model, one error taxonomy — the platform differences stay
inside the backends, never in your code.

**Version 0.2.0** — Phases 2–5 landed (see Roadmap). Apple is out
of scope since 2026-10-26 (see `docs/capabilities.md`).

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
- The Linux backend, fully implemented over libdbus (Phase 2):
  BlueZ adapter/device/GATT + NetworkManager Wi-Fi, verified
  against a fake BlueZ + NetworkManager service on a private
  D-Bus daemon — genuine protocol verification with no hardware
  (`tests/test_linux_bus`, `tests/fake_bus/`). Documented v0.2
  simplifications live in `docs/backends.md` (snapshot scans,
  synthetic GATT handles, poll-based notify, no fabricated dBm).
- Platform diagnostics + compatibility reporting (Phase 4 Linux):
  `rb_get_diagnostics()` reports backend, bus address, BlueZ /
  NetworkManager presence, adapter path/address/power, Wi-Fi
  device path, and a compatibility string.
- The Windows backend, written in full from documented WinRT APIs
  (Phase 4): BLE advertisement scan, connect, GATT, notify,
  pairing, Wi-Fi status/scan/connect. **Compile-unverified**: it
  has never been compiled (no Windows SDK on the authoring
  machine) and needs a Windows SDK + Bluetooth-capable Windows
  device to verify. Details + limits: `src/platform/windows/`.
- The Android JNI bridge + Java/Kotlin wrappers (Phase 3):
  compiled against a plain JDK and proven by a real JVM smoke test
  (`test_jni_smoke`) that loads the built library and drives the
  mock backend end to end (scan/connect/read). The Java helper
  that would call Android Framework APIs still needs an Android
  SDK + device (see `bindings/android/README.md`).
- The application layer (Phase 5, `include/radiobridge/rb_app.h`):
  device registry + capability profiles (JSON), reliable data
  transfer (framing, sequencing, retries, CRC32), firmware update
  transport (chunked, per-chunk SHA-256, manifest hash),
  OpenSSL-backed secure channels (X25519 + HKDF-SHA256 +
  AES-256-GCM; `NOT_SUPPORTED` without OpenSSL — never
  hand-rolled crypto claimed as secure), multi-device sessions,
  operation scheduling, power-aware config, connection policy
  engine.
- A cross-platform conformance suite (`tests/test_conformance.c`
  + `tests/run_conformance.sh`): the same scripted sequence
  against the mock backend and the Linux fake-bus backend,
  asserting equivalent behavior and documenting divergences.
- 16 CTest tests, all passing, covering lifecycle, capabilities,
  mock BLE scan/filter/connect/GATT/notify, mock Wi-Fi
  status/scan/connect, cancellation, error paths, callback
  threading, logging (including "passwords are never logged"),
  permissions, the Linux backend contract, mock hooks, the
  application layer, the Linux fake-bus backend, conformance,
  and the JNI smoke test.
- Two C examples (`examples/c/ble_scan.c`, `examples/c/wifi_status.c`).

## What is honestly unavailable, and why

- **Real radio hardware on this build machine**: the machine has no
  Bluetooth hardware, no Wi-Fi radio, no bluetoothd /
  NetworkManager services, and no system bus. The Linux backend is
  verified against a fake service over the real D-Bus protocol
  (not a mock shortcut), but a run against real radios on real
  hardware is still the remaining verification step.
- **Windows backend compilation** (Phase 4): written as completely
  as possible from documented WinRT APIs, but never compiled here
  (no Windows SDK). It stays marked compile-unverified until it
  builds on Windows and runs on a Bluetooth-capable device.
- **Android Framework backend** (Phase 3): the JNI bridge, Java /
  Kotlin wrappers, and JVM smoke test are compiled and proven, but
  the Java helper that calls `BluetoothLeScanner` /
  `BluetoothGatt` / `WifiManager` needs an Android SDK + device
  and does not exist yet. The Android backend itself still reports
  `NOT_SUPPORTED`.
- **Apple** (was Phase 3): out of scope since 2026-10-26. No Apple
  backend or bindings exist; `RB_BACKEND_APPLE` is retained only
  for ABI stability and always reports `NOT_SUPPORTED`.
- **Secure channels without OpenSSL**: the secure-channel functions
  return `NOT_SUPPORTED` when built without OpenSSL. No hand-rolled
  crypto is ever presented as secure.
- **Pairing and saved networks**: per-backend, see the capability
  matrix in `docs/capabilities.md` — they are reported
  `NOT_SUPPORTED` where no platform API exists, not "temporarily
  missing".

## Build, run, test

```sh
cmake --preset default        # Unix Makefiles, Release
cmake --build build -j1       # -j1: the build machine is RAM-tight
ctest --test-dir build --output-on-failure

./build/examples/example_ble_scan
./build/examples/example_wifi_status
```

Requirements: C11 compiler, CMake 3.16+, pthreads. Optional, each
enabling its feature and cleanly absent otherwise: libdbus-1-dev
(Linux D-Bus transport + fake-bus test), a JDK with `javac`/`java`
(JNI bridge + smoke test; headless JDKs work — only `jni.h` is
needed, never AWT), OpenSSL (secure channels).

## Real-hardware try-out (when you have a machine with radios)

1. Build and run the mock examples first — they prove the API shape
   without hardware.
2. For Linux: install `bluez` + `NetworkManager` + libdbus dev
   headers, make sure `bluetoothd` and NetworkManager are running,
   then run the suite — the Linux backend talks to the real
   services, and `test_linux_bus` proves the same code against the
   fake service first.
3. Hardware tests are gated: they skip, never fake, when no adapter
   is present (see `docs/testing.md`).

## Roadmap

- **Phase 1 — Foundation (done, v0.1.0)**: C11 API, capability
  model, async events, permission reporting, mock backends, Linux
  structure, documented stubs, CTest suite, docs.
- **Phase 2 — Linux transport (done, fake-bus verified)**: real
  BlueZ + NetworkManager D-Bus implementation, fake-service
  protocol verification, diagnostics. Real-hardware run still open.
- **Phase 3 — Bindings (JNI done, Framework helper blocked)**:
  compiled JNI/Java/Kotlin + JVM smoke test; Android Framework
  backend needs an Android SDK + device.
- **Phase 4 — Expansion (Linux done, Windows compile-verified by
  CI, runtime-unverified)**:
  diagnostics + compatibility reporting; full WinRT backend
  written and compiling on MSVC + Windows SDK in CI, needs a
  Windows device with radios to verify at runtime.
- **Phase 5 — Application layer (done, mock + fake-bus verified)**:
  registry/profiles, reliable transfer, firmware transport,
  OpenSSL secure channels, sessions, scheduler, power config,
  policy engine, conformance suite.
- **Later**: pairing/saved-network expansion, mesh, BLE
  advertising, Wi-Fi Direct, coexistence — only when the platform
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
