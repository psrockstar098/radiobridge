# Android backend (Phase 3)

The Android backend is a documented stub in Phase 1
(`src/platform/stubs/rb_stubs.c`): probe reports `NOT_SUPPORTED`,
`create()` fails cleanly, no operations are offered. The real backend
needs the Android SDK, a device with Bluetooth hardware, and the JNI
bridge sketched in `bindings/android/`. See `docs/backends.md` for the
error mapping and `docs/permissions.md` for the permission model.
