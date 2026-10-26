# Windows backend (Phase 3)

The Windows backend is a documented stub in Phase 1
(`src/platform/stubs/rb_stubs.c`): probe reports `NOT_SUPPORTED`,
`create()` fails cleanly, no operations are offered. The real backend
needs the Windows SDK, a device with Bluetooth hardware, and WinRT
(`Windows.Devices.Bluetooth`). Documented WinRT limits the backend
must honor: no explicit disconnect, no RSSI read, no MTU request,
connection attempts not cancellable, ~7s per queued GATT request.
See `docs/backends.md` and `docs/capabilities.md`.
