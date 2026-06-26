# Runtime Debug Viewer

Python debug viewer for runtime SHM streams.

It reads registry files, attaches to runtime streams, and draws simple 2D projections for HMD, hands, body trackers, and spatial/debug data.

## Package path

```text
out/xreal_ultra/bin/python/tools/runtime_debug_viewer/
```

## Typical use

Enable through `xr_client` with:

```bash
RUN_VIEWER=1 ./run_xr_client.sh
```

or run the viewer wrapper/config directly from the package.
