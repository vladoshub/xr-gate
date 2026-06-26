# Migration notes

Goal: keep all project-owned Basalt VIO code under:

```text
xr_tracking/backends/basalt_vio/
```

and keep upstream Basalt modifications as a small patch under:

```text
xr_tracking/backends/basalt_vio/patches/basalt/
```

Recommended ownership split:

```text
xr_tracking/backends/basalt_vio/    project-owned Basalt VIO backend code
xr_tracking/shared/include/         common transport/runtime headers
~/src/basalt                           upstream Basalt checkout + small integration patch only
```

`capture_client` stays shared because it is not Basalt-specific.
