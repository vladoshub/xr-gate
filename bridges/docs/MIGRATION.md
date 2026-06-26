# Migration from old flat `src/` layout

Moved into `bridges/`:

```text
src/capture_net_bridge.cpp              -> bridges/capture_net/src/capture_net_bridge.cpp
src/tracking_udp_bridge.cpp             -> bridges/tracking_udp/src/tracking_udp_bridge.cpp
src/tracking_udp_debug_receiver.cpp     -> bridges/tracking_udp/src/tracking_udp_debug_receiver.cpp
```

After applying this layout and building successfully, the old flat copies can be removed.

Safe cleanup dry-run:

```bash
cd ~/src/xr_tracking
bridges/scripts/linux/cleanup_old_flat_sources.sh
```

Actual deletion of safe candidates:

```bash
REMOVE=1 bridges/scripts/linux/cleanup_old_flat_sources.sh
```

Review manually before deleting:

```text
src/xr_runtime_adapter.cpp
src/capture_stereo_rate_check.cpp
src/CMakeLists.txt
```

Recommended next moves:

- move `src/xr_runtime_adapter.cpp` into a dedicated runtime adapter component;
- move `src/capture_stereo_rate_check.cpp` into capture-service/debug tooling or delete if superseded;
- remove `src/CMakeLists.txt` after no sources remain in the flat `src/` directory.
