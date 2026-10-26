# Apple backend (Phase 3)

The Apple backend is a documented stub in Phase 1
(`src/platform/stubs/rb_stubs.c`): probe reports `NOT_SUPPORTED`,
`create()` fails cleanly, no operations are offered. The real backend
needs Xcode, an Apple device or Mac, Core Bluetooth, and the Network
framework; wrappers are sketched in `bindings/apple/`. Note the Apple
identifier difference: peripherals are identified by UUID, not MAC
address. See `docs/backends.md` and `docs/capabilities.md`.
