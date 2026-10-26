# Windows backend (Phase 4)

COMPILE-UNVERIFIED — written from documented WinRT APIs, never
compiled (no Windows SDK on the authoring machine). Requires the
Windows SDK + a Bluetooth-capable Windows device to verify. The
probe detail string reports COMPILE-UNVERIFIED at runtime.

## Files

- `rb_windows.c` — C11 vtable shim (`rb_windows_backend_ops`);
  RSSI and Wi-Fi quality answer NOT_SUPPORTED without touching WinRT.
- `rb_windows.cpp` — BLE: advertisement scan, connect with managed
  reconnect, GATT discover/read/write/subscribe/unsubscribe, pairing
  helpers, probe, create/destroy.
- `rb_windows_wifi.cpp` — Wi-Fi: probe, connectivity-profile status,
  scan loop, connect worker, disconnect.
- `rb_windows_internal.h` — C-linkage bridge (no WinRT types).
- `rb_windows_state.h` — C++-only shared state and sync-call machinery.
- `rb_windows_errors.md` — error-mapping tables, capability table,
  limitations, first-build checklist, unverified-API list.

Language: C++17 with C++/WinRT behind an `extern "C"` shim; the
public library stays C11. Only Windows builds compile these files
(`#ifndef _WIN32 #error` guards + the `if(WIN32)` CMake gate);
non-Windows builds keep the honest stub in
`src/platform/stubs/rb_stubs.c` for explicit `RB_BACKEND_WINDOWS`.

## Documented WinRT limits (see `rb_windows_errors.md`)

- No explicit disconnect (clear `MaintainConnection`, close handles;
  the OS drops the link when no app holds the device).
- No connected-device RSSI read (scan results carry advertisement RSSI).
- Connection attempts and GATT requests are not cancellable.
- Wi-Fi needs the `wifiControl` manifest capability: a packaged
  (MSIX/sparse-package) consumer gets full Wi-Fi; an unpackaged Win32
  desktop app has no manifest to declare it in and will see
  REQUIRES_PERMISSION / PERMISSION_DENIED (needs on-device
  confirmation).
- Wi-Fi security is always UNKNOWN (`WiFiAvailableNetwork` exposes no
  auth info); no IP/gateway in status; service handles are 0
  (WinRT exposes `AttributeHandle` only on characteristics/descriptors).
