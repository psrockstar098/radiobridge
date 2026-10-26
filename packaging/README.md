# Packaging

- `radiobridge.pc.in` — pkg-config template (filled in by CMake's
  `configure_file` once an install target is added in Phase 2).
- No install rules yet: Phase 1 ships as a build-tree static library
  (`build/libradiobridge.a`). Install targets, shared-library builds,
  and packaging (deb/rpm/zip) land with Phase 2 once the Linux
  transport exists and CI is green.
- `RB_BUILD_SHARED` already toggles a shared-library build.
