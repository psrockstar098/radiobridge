# Events (Phase 1)

The async event system lives in the core in Phase 1:
`src/core/rb_core.c` (operation registry, event dispatch, callback
rules). This directory reserves the split for when the event layer
grows its own module (e.g. an event queue + dedicated dispatch
thread). See `docs/api.md` for the callback contract: callbacks fire
from internal threads, structs are valid only during the callback,
never call `rb_shutdown()` from inside one.
