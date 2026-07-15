# Legacy Monado environment profiles

The primary display/optics format is now YAML under:

```text
configs/display/default.yaml
```

Select a YAML profile with:

```bash
XR_MONADO_DISPLAY_CONFIG=/path/profile.yaml bin/drivers/monado_driver/start.sh
```

These `.env` profiles remain as a compatibility layer for existing launchers.
They are loaded after YAML and should use `${VAR:-value}` so explicit caller
values and values supplied by the YAML profile remain authoritative.
