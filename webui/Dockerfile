# Kublet Fleet WebUI — port 1666
#
# Bundles the kublet-apps repo + PlatformIO + dev tool so a single container
# can build and OTA-deploy any app to any registered kublet on the LAN.
#
# Build:    docker build -t kublet-fleet .
# Run:      docker run --rm -p 1666:1666 \
#             -v "$PWD/devices.json:/srv/webui/devices.json" \
#             -v "$PWD/data:/root/.platformio" \
#             --network host \
#             kublet-fleet
#
# --network host (or attaching to the same LAN bridge) is needed so the
# container can reach the kublets at their LAN IPs to push OTA firmware.

FROM python:3.13-slim

ENV DEBIAN_FRONTEND=noninteractive \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PIP_NO_CACHE_DIR=1

# Toolchain prerequisites for PlatformIO ESP32 builds
RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        ca-certificates \
        build-essential \
        curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /srv

# Bring in the kublet-apps repo (cloned/copied at build time from the build context)
COPY kublet-apps /srv/kublet-apps

# Install dev tool + PlatformIO into the system Python
RUN pip install -e /srv/kublet-apps platformio flask

# Pre-warm PlatformIO platforms + libraries by doing one stock build,
# so the first user-triggered deploy doesn't pay the cold-start cost
RUN cd /srv/kublet-apps && python tools/dev build silver || true

COPY webui /srv/webui
WORKDIR /srv/webui

EXPOSE 1666
CMD ["python", "app.py"]
