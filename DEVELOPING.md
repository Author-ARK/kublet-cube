# Developing on Kublet-Asu

Practical workflow guide for anyone hacking on the repo — how to run the
fleet web-UI, the desktop emulator, push firmware to a cube, and add a
new app. For the original "get a cube on your network for the first
time" walkthrough see [HOWTO.md](HOWTO.md).

---

## 1. Prerequisites

- **macOS 12+** (or Linux — see [HOWTO.md → Linux note](HOWTO.md#linux-note)).
- **Homebrew**, **git**, **Python ≥ 3.12** on `PATH`.
- The **SiLabs CP210x VCP driver** for USB-serial access to the cube —
  install from <https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers>.
- About **1 GB** free for the ESP32 toolchain (PlatformIO downloads it
  the first time you build).

## 2. One-shot install

```bash
git clone https://github.com/<you>/kublet-asu.git
cd kublet-asu
bash install.sh
```

`install.sh` is idempotent — it:

1. installs `cmake`, `sdl2`, `ffmpeg` via Homebrew if missing,
2. clones the [`markusos/kublet-apps`](https://github.com/markusos/kublet-apps)
   community fork into `kublet-apps/`,
3. creates a Python virtualenv at `kublet-apps/kublet_env/`,
4. installs `pio`, `esptool`, `pyserial`, `flask` into that venv,
5. patches a small bug in the desktop emulator's CMake config,
6. runs one smoke build (~5 min the first time, mostly toolchain
   download — much faster afterwards).

If anything blows up the script dumps the last 20 lines of the build log.

## 3. Running the fleet web-UI

The webui is a Flask app on port **1666**. Lifecycle is wrapped in
`./kublet.sh`:

```bash
./kublet.sh start      # boot in the background, log → /tmp/kublet-webui.log
./kublet.sh status     # PID, uptime, any active deploy subprocesses, log tail
./kublet.sh logs       # tail -f the log
./kublet.sh stop       # graceful TERM + sweep hung pio/esptool children
./kublet.sh restart    # stop + start
```

Then open <http://localhost:1666>. Tabs:

| Tab              | Use it for                                                 |
| ---------------- | ---------------------------------------------------------- |
| ⚡ Flash me      | Burn dev firmware + WiFi creds onto a brand-new cube over USB |
| 📡 Devices       | Per-cube name / number / IP, live online dot, edit / remove  |
| 🚀 Deploy        | Build an app and OTA-push it (or 🔌 USB fallback)             |
| 🖥 Simulator     | Watch the SDL2 emulator's framebuffer live in the browser    |
| 📱 Apps catalog  | Preview tiles, sort recent / A→Z / Z→A                       |
| ℹ About          | Credits + hardware specs + recovered docs                    |

The webui never persists secrets — WiFi creds live only in `tools/.env`
(gitignored) and on each cube's own NVS partition; device IPs live in
`webui/devices.json` (also gitignored).

## 4. Desktop emulator

The emulator builds any app for the host instead of the cube — much
faster iteration on layout. Run from `kublet-apps/`:

```bash
cd kublet-apps && source kublet_env/bin/activate
./tools/emulate Crypto-BTC                       # interactive SDL window
./tools/emulate Crypto-BTC --screenshot out.png  # headless one-shot
./tools/emulate Metal-Au --gif clip.gif --gif-duration 6
```

Keys while the SDL window is open: **Space** = press button, **S** =
save `screenshot.png`, **Q** = quit.

The fleet web-UI also exposes the emulator as a 💻 *sim slot* — add a
device of kind "emulator" in the 📡 Devices tab, then deploy any app to
it from 🚀 Deploy. The **🖥 Simulator** tab streams the cube's framebuffer
into the browser via PNG dumps the emulator writes every 200 ms.

> The emulator uses *fixtures* in each app's `assets/http_fixtures.json`
> to serve canned API responses. To capture a fresh fixture from a real
> endpoint, just curl it into `assets/<file>.json` and reference it from
> `http_fixtures.json`.

## 5. Deploying to a cube

### OTA (over WiFi, default)

From the webui: 🚀 Deploy tab → pick app + target cube → "Build + deploy".
From the CLI:

```bash
cd kublet-apps && source kublet_env/bin/activate
./tools/dev deploy <app> <device-name>           # device from registry
./tools/dev deploy <app> --ip 192.168.x.x        # explicit IP override
```

### USB fallback (when WiFi to that cube is flaky)

The webui's 🔌 USB fallback checkbox routes the deploy through esptool
on the cable instead. CLI equivalent:

```bash
./tools/dev deploy <app> --usb -p /dev/cu.usbserial-XXXX
```

The USB path is *OTA-style smart-slot*: reads the cube's `otadata`
partition, writes firmware to the *inactive* slot, then bumps the
pointer — exactly what Arduino's `Update.h` does for OTA. If `read_flash`
fails (some cubes' stub-loader handshake is noisy), it falls back to
the simple "write `ota_0` + clear `otadata`" path, which always works.

## 6. Recovering a bricked cube

If a cube boot-loops or shows nothing, the safest recovery is to
re-flash the dev firmware via USB and start over:

```bash
./tools/dev init --name kublet-X -p /dev/cu.usbserial-XXXX --no-wait
```

That rewrites bootloader + partitions + NVS + dev firmware. After that
re-deploy any app.

## 7. Adding a new app

The simplest path is to clone an existing one:

```bash
cd kublet-apps/apps
cp -R Crypto-BTC My-New-App
# 1. Edit My-New-App/manifest.yaml — change summary / desc
# 2. Edit My-New-App/src/main.cpp — change COIN_TICKER / COIN_SYMBOL /
#                                   COIN_COLOR (or whatever the app
#                                   exposes as constants)
# 3. Optionally edit My-New-App/assets/http_fixtures.json for emulator
#    testing
./tools/emulate My-New-App                       # iterate on layout
./tools/dev deploy My-New-App <device>           # then push to a cube
```

A few conventions the existing apps follow:

- `manifest.yaml` carries `summary:` (one-line, shown in the catalog
  tile) and `desc:` (longer, used in the about / tooltip text).
- `assets/preview.png` is the gallery thumbnail. Generate it via
  `./tools/emulate <app> --screenshot assets/preview.png --after 3`.
- The OTA listener wiring at the top of `setup()` and `loop()` is
  marked **`// DO NOT EDIT`** in every app — leave those two lines alone
  or your cube won't accept further OTA pushes.
- Apps that fetch network data should mark the green/red dot in their
  footer based on `lastFetchOk` so the cube's proof-of-life signal
  matches the rest of the fleet.

## 8. Troubleshooting cheat sheet

| Symptom                                 | First thing to try                                                                  |
| --------------------------------------- | ----------------------------------------------------------------------------------- |
| No `/dev/cu.usbserial-*` appears        | Bad cable (charge-only); SiLabs CP210x driver missing; USB hub interfering          |
| OTA push stalls partway                 | `ping <cube-ip>` — if RTT > 20 ms or has loss, WiFi link is too weak. Use 🔌 USB    |
| Cube doesn't reboot after USB flash     | RTS-pin reset not wired — unplug + replug USB cable to force a clean power cycle    |
| Emulator build fails on `print(String)` | Mock missing the overload — see commit history for `mock/TFT_eSPI.h::print(String&)`|
| Webui won't bind to :1666               | Another process already on the port: `lsof -t -i :1666` then kill, or `./kublet.sh restart` |
| "0x7F invalid head of packet" on flash  | esptool stub-loader noise — already handled by the `--no-stub` fallback in the dev tool |

## 9. Repository conventions

- **`.gitignore` is authoritative for secrets.** If you're adding a new
  file type that could carry credentials (e.g. a `.env.local`, an
  exported API key), add a pattern there before committing.
- License headers aren't required on individual files — the root
  `LICENSE` (Apache 2.0) and `NOTICE` cover the whole repository.
- New external dependencies / data sources go into `NOTICE` so we don't
  forget who built what we're standing on.

Questions: open an issue, or email
[schlaptop@protonmail.com](mailto:schlaptop@protonmail.com).
