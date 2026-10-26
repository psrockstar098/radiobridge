# Tests

The Phase 1 suite is the `test_*.c` files in this directory (see
`docs/testing.md` for what each covers). Subdirectories reserve the
future split:

- `unit/` — unit tests (the current suite will move here in Phase 2).
- `mocks/` — mock-backend fixtures and catalog definitions.
- `integration/` — cross-backend tests, hardware-gated (skip, never
  fake, when hardware is absent).
- `hardware/` — real-radio tests, run only on machines with an
  adapter; never run in CI.
