# xr_runtime_adapter platform layer

This directory contains the small platform policy layer used by
`xr_runtime_adapter.cpp`.

- `linux/` keeps the current POSIX SHM defaults and existing Linux runtime path.
- `windows/` selects native Windows-safe defaults and rejects POSIX SHM options
  early with a clear error. Windows runtime publishing should use UDP/TCP paths.

The main adapter logic should stay platform-neutral. Add Linux/Windows behavior
here instead of putting new `#ifdef _WIN32` blocks directly into
`xr_runtime_adapter.cpp`.
