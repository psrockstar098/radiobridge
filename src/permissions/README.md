# Permissions (Phase 1)

Permission reporting lives in the core in Phase 1:
`src/core/rb_core.c` (`rb_get_required_permissions()`). This
directory reserves the split for when per-platform permission tables
grow their own module. The rule never changes: the library REPORTS
required permissions; it NEVER requests them, and it NEVER logs
secrets. See `docs/permissions.md`.
