# Kublet Silver — How To

A custom firmware for the [Kublet cube](https://thekublet.com) (the small
ESP32 desk-display that got abandoned in mid-2024). Shows the live silver
spot price (USD per troy ounce), refreshes every minute, dims itself by
local time of day. Built on the [community fork by Markus
Östberg](https://github.com/markusos/kublet-apps).

> **No iOS app, no Android app, no Kublet account required.** You flash
> the cube directly from your Mac over USB once, after that updates go
> over WiFi.

---

## What you need

- A **Kublet cube** (any of the units shipped before the company went dark).
- A **Mac** with macOS 12 or newer. (Linux works too — see
  [Linux note](#linux-note) at the bottom.)
- A **USB Mini-B cable** that does **data**, not just power. The Kublet's
  port is the older trapezoid USB Mini-B connector — *not* USB-C, *not*
  Micro-USB. Most likely you have a USB-C-to-Mini-B or USB-A-to-Mini-B
  cable. Many old Mini-B cables shipped charge-only (cameras, GPS, MP3
  players from that era) — if your Mac doesn't see a serial port, that's
  the most likely cause. Borrow a known-good data cable to rule it out.
- The **SiLabs CP210x VCP driver** for macOS, from
  <https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers>.
  The cube's USB-serial chip is a Silicon Labs CP210x; without the driver,
  macOS powers the cube but never exposes a `/dev/cu.*` node.
- Your **WiFi SSID + password**.
- About **1 GB free disk** (most of it is the ESP32 toolchain that
  PlatformIO downloads on first build).
- ~15 minutes for the first install (faster on subsequent cubes).

---

## Step 1 — Install

In a terminal:

```bash
cd ~/Documents             # or wherever you want it
bash install.sh
```

The installer:

1. checks Homebrew + git + Python ≥3.12,
2. installs `cmake`, `sdl2`, `ffmpeg` if missing (needed for the desktop
   emulator),
3. clones [`markusos/kublet-apps`](https://github.com/markusos/kublet-apps),
4. creates a Python virtualenv at `kublet-apps/kublet_env/`,
5. installs `pio`, `esptool`, `pyserial`, `flask` into that venv,
6. patches a small bug in the desktop emulator's CMake setup,
7. runs one smoke build (~5 min the first time, mostly toolchain
   download).

Re-running `install.sh` is safe — it skips anything already in place.

If the smoke build fails, the script dumps the last 20 lines of the build
log; usually it's a missing brew package or a network blip.

---

## Step 2 — First flash (USB)

Plug the cube into your Mac. macOS exposes its serial port as something
like `/dev/cu.usbserial-XXXX` or `/dev/cu.SLAB_USBtoUART`. You don't
need to know the path — the dev tool autodetects it.

```bash
cd ~/Documents/kublet-silver/kublet-apps        # adjust path
source kublet_env/bin/activate
python tools/dev init --name kublet1
```

You'll be prompted for your **WiFi SSID** and **password**. Both go
into the cube's NVS flash so it remembers them across reboots, and
they never leave your machine. The dev tool then flashes the *dev
firmware* (`firmware/dev_firmware.bin`) which is the OTA-listening
shell that all subsequent app deploys plug into.

When it finishes you'll see a serial log line like:

```
NTP synced: 2026-05-15 11:24 (-03 Asuncion)
[OTA] listening on 192.168.1.50
```

That IP is what the OTA pushes target. The dev tool also writes
`kublet1: 192.168.1.50` into `.devices.yml` so you don't have to
remember it.

---

## Step 3 — Deploy the silver app

Still in the venv:

```bash
python tools/dev deploy silver kublet1
```

About 30 seconds: builds the firmware, MD5-checks it, pushes to the
cube over WiFi, the cube reboots into the silver app. Output looks
like:

```
Building 'silver'...
  ✓ Build succeeded (1039 KB)
📡 Sending 'silver' (1039 KB) to 192.168.1.50...
  🔒 Firmware MD5: 6ad2f5...
  ████████████████████ 100% (1039/1039 KB)
  ✓ Device back online
```

On the screen you should see:

```
┌────────────────────────────────────┐
│  AG - SILVER PRICE         #1      │
│       2026-05-15  14:32:00         │
├────────────────────────────────────┤
│                                    │
│         $ 33.49                    │
│                                    │
│      USD per troy ounce            │
├────────────────────────────────────┤
│  updated 14:32              ●      │
└────────────────────────────────────┘
```

The `#1` in the corner is this cube's identifier. You'll set a
different number on each kublet so you can tell them apart.

---

## Step 4 — More cubes (optional)

For each additional cube:

```bash
python tools/dev init --name kublet2     # 3, 4, ...
```

Then in `webui/devices.json` (or via the webui form, see Step 5)
register the new cube with `num: 2`, `num: 3`, etc. Each deploy
rebuilds the firmware with the per-cube `KUBLET_NUM` baked in, so
all your cubes show their own number.

---

## Step 5 — Fleet webui (port 1666)

When you have more than one cube, the webui beats typing CLI commands.

```bash
cd ~/Documents/kublet-silver/webui
~/Documents/kublet-silver/kublet-apps/kublet_env/bin/python app.py
# → http://localhost:1666
```

The page lets you:

- register cubes (name + IP + number),
- pick an app + a target cube,
- one-click "Build + OTA deploy" with live build log,
- remove cubes you no longer have.

To run it as a Docker service so it stays up when you close the
terminal:

```bash
cd ~/Documents/kublet-silver
docker build -t kublet-fleet -f webui/Dockerfile .
docker run -d --name kublet-fleet --restart unless-stopped \
    --network host \
    -v "$PWD/webui/devices.json:/srv/webui/devices.json" \
    kublet-fleet
```

`--network host` is required so the container can talk to cubes on
your LAN.

---

## Troubleshooting

**`/dev/cu.usbserial-*` not appearing.** Bad cable. Try another (data,
not charge-only). On Apple-Silicon Macs you may also need the SiLabs
[CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).

**`pio` not found when running `tools/dev`.** You forgot `source
kublet_env/bin/activate`. The dev tool does `shutil.which("pio")` so
PATH must include the venv `bin`.

**Yahoo Finance returns nothing for an hour.** Rare rate limit. The
fetcher in `apps/silver/src/main.cpp` is deliberately isolated; swap
in [Stooq's CSV endpoint](https://stooq.com/q/?s=xagusd) instead.
About 10 lines of change.

**Brightness doesn't change between day/night.** GPIO 15 is the
backlight. Check your cube's serial log for `ledc` errors. Brightness
schedule is hardcoded in `computeBrightness()`:
- 08:00–18:00 → bright (255)
- 06:00–08:00, 18:00–22:00 → normal (128)
- 22:00–06:00 → low (40)
All in **Asunción local time** (UTC-3, no DST since Oct 2024). Edit
the function for a different schedule.

**`docker build` fails with "context does not contain kublet-apps".**
Run `docker build` from the parent directory (`9-Kublet/` or wherever
the installer dropped things), not from inside `webui/`.

---

## Linux note

Everything except the Homebrew step works on Linux. Replace step 1
of the installer with your distro's package manager:

```bash
sudo apt install build-essential cmake libsdl2-dev ffmpeg python3-venv git
git clone https://github.com/markusos/kublet-apps
cd kublet-apps && python3 -m venv kublet_env && source kublet_env/bin/activate
pip install -e . platformio flask
```

Serial port on Linux is usually `/dev/ttyUSB0`; you may need to
`sudo usermod -a -G dialout $USER` and log out/in to access it.

---

## What lives where

```
~/Documents/kublet-silver/
├── install.sh                  ← one-shot bootstrap
├── HOWTO.md                    ← this file
├── HANDOFF.md                  ← what's done / what needs hardware
├── kublet-apps/                ← cloned community fork
│   ├── apps/silver/            ← the silver app source
│   ├── kublet_env/             ← Python venv (Dropbox-ignored)
│   ├── tools/dev               ← build / init / deploy CLI
│   └── tools/emulate           ← desktop SDL2 emulator
└── webui/                      ← Flask UI on port 1666
    ├── app.py
    ├── templates/index.html
    ├── static/style.css
    ├── devices.json            ← persistent device registry
    └── Dockerfile
```

That's it.
