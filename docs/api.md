# RadioBridge public API contract

Version 0.1.0. Headers: `include/radiobridge/rb.h` (umbrella +
lifecycle), `rb_types.h` (data model), `rb_bluetooth.h`,
`rb_wifi.h`, `rb_errors.h`. Test-only mock hooks live in `rb_mock.h`
and are NOT part of the stable ABI.

## Lifecycle

```c
rb_initialize(&config, &ctx);   /* config may be NULL for defaults */
... use ctx ...
rb_shutdown(ctx);               /* exactly once; pointer invalid after */
```

`RB_BACKEND_AUTO` prefers the native backend for the host and falls
back to the mock backend (with a warning log) when native is
unavailable. An explicitly requested backend that cannot run fails
`rb_initialize()` with `RB_ERROR_UNAVAILABLE` (service absent) or
`RB_ERROR_NOT_SUPPORTED` (platform can never run it here).

## ABI rules

- C11, plain structs with fixed-size arrays. No C++ types, no
  exceptions, no STL, no compiler-specific layout across the boundary.
- No platform object ever crosses the boundary: no `CBPeripheral`, no
  Android `BluetoothDevice`, no WinRT handle, no D-Bus proxy. Backends
  translate native objects into portable structs immediately.
- Strings are UTF-8, NUL-terminated, inside fixed buffers.
- Error codes are appended only, never renumbered
  (`rb_status_string()` is stable).
- Version: `rb_version_string()` (`"0.1.0"`), `rb_abi_version()`
  (packed major/minor/patch).

## Ownership and lifetimes

- Structs handed to a callback are owned by the library and valid
  ONLY during that callback. Copy what you need.
- Arrays returned by synchronous functions (`rb_ble_discover()`) are
  heap-allocated; free them with `rb_free()`.
- Passwords passed to `rb_wifi_connect()` are used only for the
  attempt, never logged, never retained (covered by `test_logging`).
- Operation ids (`rb_operation_id_t`) are unique per context; `0` is
  never valid. Cancelled/finished ids return `RB_ERROR_NOT_FOUND`.

## Threading

- Every public function is safe to call from any thread.
- Callbacks fire from internal threads and must be thread-safe.
- Never call `rb_shutdown()` from inside a callback. Never stop your
  own scan from inside a scan callback (stop from another thread, or
  use `rb_cancel()` from the owning thread).
- Synchronous GATT calls (discover/read/write) block briefly on the
  calling thread while the backend resolves them. Scans, connects, and
  notifications are asynchronous by design: radio work must never
  block application UI threads.

## Cancellation

`rb_cancel(ctx, op)` flags the operation; backends check the flag
between deliveries (mock: promptly) or map it to the platform cancel
where one exists. Where the platform has no cancel primitive (WinRT
connection attempts are famously not cancellable), the operation
completes or times out and the backend documents that. A cancelled
connect delivers a `FAILED` event with `RB_ERROR_CANCELLED`.

## Errors

| Code | Meaning |
| ---- | ------- |
| `RB_OK` | success |
| `RB_ERROR_INVALID_ARGUMENT` | NULL pointer, bad enum, malformed id |
| `RB_ERROR_NOT_INITIALIZED` | NULL or shut-down context |
| `RB_ERROR_NO_MEMORY` | allocation failure |
| `RB_ERROR_NOT_SUPPORTED` | backend can never do this |
| `RB_ERROR_UNAVAILABLE` | backend/service absent right now |
| `RB_ERROR_PERMISSION_DENIED` | OS permission missing/denied |
| `RB_ERROR_NOT_FOUND` | device, network, service, characteristic unknown |
| `RB_ERROR_TIMEOUT` | operation timed out |
| `RB_ERROR_CANCELLED` | cancelled via `rb_cancel()` |
| `RB_ERROR_ALREADY_IN_PROGRESS` | second scan/connect while one runs |
| `RB_ERROR_NOT_CONNECTED` | operation needs a connection |
| `RB_ERROR_BUSY` | backend busy (e.g. out of scan resources) |
| `RB_ERROR_AUTH_FAILED` | pairing / Wi-Fi auth failed |
| `RB_ERROR_INTERNAL` | backend bug or unexpected native error |

## Async vs sync surface

- Async (callback + operation id): BLE scan, BLE connect, Wi-Fi scan.
  BLE state changes arrive via the connection callback; Wi-Fi changes
  via the general event callback (`RB_EVENT_WIFI_CHANGED`).
- Sync (returns directly): GATT discover/read/write, subscribe setup,
  RSSI, Wi-Fi status, Wi-Fi connect request acceptance (the outcome
  surfaces via status/events), quality measurement.
- Notifications/indications stream via the notify callback until
  unsubscribe or disconnect.
