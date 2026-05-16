# Kublet Silver — Hardware Bring-Up

What's done in software, what you need to do at the cube. Treat this as the
"next time you sit down with a kublet" cheat sheet.

## What's already built

- `kublet-apps/apps/silver/` — silver-price app (header `AG - SILVER PRICE`,
  per-device `#N` in upper-right, big USD/oz price, auto-dim by Asunción time,
  60 s refresh from Yahoo Finance `chart/SI=F`).
- Firmware compiled successfully (1039 KB / 80 % of flash), verified in the
  desktop emulator. Screenshot at `/tmp/silver_screenshot.png` from the last
  emulator run.
- Python venv at `kublet-apps/kublet_env/` with `pio`, `esptool`, `pyserial`,
  `flask`, `kublet_dev` — everything the dev tool needs.
- Webui at `webui/` running on **port 1666** (Flask) — register devices, pick
  app, OTA-deploy. Dockerfile included.
- Patches to upstream community fork (kept local; not pushed):
  - `tools/src/emulate/CMakeLists.txt` — SDL2 `IMPORTED_TARGET` fix so the
    emulator builds on Apple-silicon brew.
- Memory captured for future sessions: project goal, fork tooling quirks.

## What I could not do (needs you + the cube)

1. **Flash WiFi credentials** — needs a data-capable **USB Mini-B cable** (the trapezoid connector, not USB-C). Most cubes show "open mobile app" until this is done.
2. **Push firmware** — needs the cube on your LAN.
3. **Confirm visual layout on real ST7789 panel** — emulator is faithful but
   the real LCD's gamma/colors will look slightly different.
4. **Verify backlight PWM** — auto-dim is wired to GPIO 15 ledc channel 0;
   stubbed in the emulator. First time you run it, check the brightness
   actually changes between bright/normal/low.

## Step 1 — One-time WiFi flash via USB

Plug the cube into your Mac with a data-capable USB Mini-B cable, with the
SiLabs CP210x driver installed. The serial port shows up as
`/dev/cu.usbserial-XXXX` or `/dev/cu.SLAB_USBtoUART`.

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/kublet-apps
source kublet_env/bin/activate
python tools/dev init --name kublet1
```

You'll be prompted for SSID + password. Tool writes them to NVS via serial,
flashes the dev firmware (the one in `firmware/dev_firmware.bin`), and
registers the device in `.devices.yml` as `kublet1`. After this the cube
boots, joins WiFi, and is reachable for OTA pushes.

When it comes back up, note the IP printed on the serial monitor (or
discoverable via `python tools/dev devices`).

## Step 2 — First OTA deploy (CLI)

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/kublet-apps
source kublet_env/bin/activate
python tools/dev deploy silver kublet1
# or with explicit IP:
python tools/dev deploy silver --ip 192.168.x.x
```

The deploy takes ~30 s. The cube reboots into the silver app. You should see
the `AG - SILVER PRICE` header, `#1` in the corner, and the big silver
price. If it shows `$ --.--` for more than a minute, Yahoo Finance is rate
limiting (rare) — fallback API noted at the bottom of this file.

## Step 3 — Use the webui (port 1666)

Once you have at least one kublet on the LAN:

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/webui
/Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/kublet-apps/kublet_env/bin/python app.py
# → http://localhost:1666
```

The page shows the registered devices (initially empty), an add-device form,
an app dropdown, and a deploy button. The webui shells out to
`tools/dev deploy ... --ip ...` with `PLATFORMIO_BUILD_FLAGS=-DKUBLET_NUM=N`
so each cube gets its own number baked in.

To run the webui in Docker (Phase 2 portability):

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet
docker build -t kublet-fleet -f webui/Dockerfile .
docker run --rm --network host \
    -v "$PWD/webui/devices.json:/srv/webui/devices.json" \
    kublet-fleet
```

`--network host` is required so the container can reach kublets on the LAN.

## Step 4 — Add more kublets

For each additional cube:

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/kublet-apps
source kublet_env/bin/activate
python tools/dev init --name kublet2     # then 3, 4, ...
```

Then add it in the webui (or edit `webui/devices.json` directly) with
`num: 2`, `num: 3`, etc. Deploys from the webui will bake the correct
`#N` into each cube's firmware automatically.

## If the price doesn't load

Yahoo Finance occasionally blocks unauthenticated polling. The fetch lives
in [`apps/silver/src/main.cpp`](kublet-apps/apps/silver/src/main.cpp) —
`fetchSilverPrice()`. Drop-in replacement endpoint (no auth):

```
http://stooq.com/q/l/?s=xagusd&f=sd2t2ohlc&h&e=csv
```

Returns CSV: `XAGUSD,2026-05-15,21:45:21,33.45,33.49,33.44,33.49`. Parse the
6th comma-separated field as the close price. Switch is ~10 lines.

## Future (queued)

- gold (`XAU`), fiat currencies, crypto, weather (forecast / pressure /
  temp / rain / sun / moon).
- NVS-stored `KUBLET_NUM` so the same firmware binary works on every cube
  without per-device rebuild.
- iOS + Android companion apps.

See the todo list in this session and `.claude/projects/-Users-schlaptop/memory/`
for the persisted scope.
