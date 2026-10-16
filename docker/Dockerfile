# syntax=docker/dockerfile:1
FROM --platform=linux/amd64 ubuntu:24.04

LABEL org.opencontainers.image.title="XR Gate TCP runtime" \
      org.opencontainers.image.source="https://github.com/vladoshub/xr-gate" \
      org.opencontainers.image.version="v2.3.0"

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl python3 && rm -rf /var/lib/apt/lists/*

COPY release.lock.json /usr/local/share/xr-gate/release.lock.json
COPY image/install-release.py /usr/local/lib/xr-gate/install-release.py
RUN python3 /usr/local/lib/xr-gate/install-release.py

# Use the dependency list from the SAME release. No build tools or source build.
# Suppress service starts in the image; BlueZ runs on the Linux host.
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && \
    chmod +x /usr/sbin/policy-rc.d && \
    XR_PACKAGE_ROOT=/opt/xr-gate \
      /opt/xr-gate/devices/common/linux/scripts/runtime/install_runtime_deps_ubuntu.sh \
      --no-device-access && \
    /opt/xr-gate/bin/runtime_adapters/xr_runtime_adapter/xr_runtime_adapter --help \
      > /tmp/adapter-help && grep -q -- '--app-tcp' /tmp/adapter-help && \
    rm -f /tmp/adapter-help && rm -rf /var/lib/apt/lists/*

COPY image/entrypoint.sh /usr/local/bin/xr-gate-entrypoint
COPY image/check-runtime.py /usr/local/lib/xr-gate/check-runtime.py
COPY scripts/tcp_probe.py /usr/local/lib/xr-gate/tcp_probe.py
RUN chmod +x /usr/local/bin/xr-gate-entrypoint && \
    mkdir -p /config /root/.config/xr_tracking/override_controller /tmp/xr_backend_client_logs

ENV XR_ROOT_PROJECT=/opt/xr-gate \
    ROOT_PROJECT=/opt/xr-gate \
    XR_PROFILE=xreal_ultra \
    PYTHONUNBUFFERED=1 \
    AUTO_BUILD=0
WORKDIR /opt/xr-gate
EXPOSE 45810/tcp
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/xr-gate-entrypoint"]
