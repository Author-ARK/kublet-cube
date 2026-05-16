# Kublet Fleet WebUI

Port **1666**. Browser-driven interface to register your kublets, then build + OTA-deploy any app from `../kublet-apps/apps/` to any of them. Each device gets a number (`#N`) baked into the firmware via `-DKUBLET_NUM=N` so the same source compiles personalised for each cube.

## Run locally (no Docker)

The webui shells out to `../kublet-apps/tools/dev`, which expects PlatformIO + esptool + pyserial in a venv. Reuse the existing one at `../kublet-apps/kublet_env/`:

```bash
cd ../kublet-apps && source kublet_env/bin/activate && pip install flask
cd ../webui
python app.py
# → http://localhost:1666
```

## Run in Docker

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet
# Build context must contain both kublet-apps/ and webui/
docker build -t kublet-fleet -f webui/Dockerfile .
docker run --rm --network host \
    -v "$PWD/webui/devices.json:/srv/webui/devices.json" \
    kublet-fleet
```

`--network host` lets the container reach kublets at their LAN IPs to push OTA. The first build inside Docker takes several minutes (downloading the ESP32 toolchain); the Dockerfile pre-warms it with one `silver` build.

## Adding devices

Either through the UI (form at the bottom of the devices table) or by editing `devices.json` directly:

```json
{
  "devices": [
    { "name": "kitchen", "ip": "192.168.1.50", "num": 1 },
    { "name": "office",  "ip": "192.168.1.51", "num": 2 }
  ]
}
```

## How it works

`POST /api/deploy {app, device}` spawns a subprocess:

```bash
PLATFORMIO_BUILD_FLAGS="-DKUBLET_NUM=<num>" \
    python ../kublet-apps/tools/dev deploy <app> --ip <device.ip>
```

Logs stream into the in-memory job store; the page polls `/api/jobs/<id>` once per second and renders the live tail.

## Files

- `app.py` — Flask server, port 1666
- `templates/index.html` — single-page UI
- `static/style.css` — dark theme
- `devices.json` — persistent device registry (mount as a volume in Docker)
- `Dockerfile` — production image
